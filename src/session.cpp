#include "curlinho/session.h"
#include "curl/curl.h"
#include "curlinho/util.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>

namespace curlinho {

Session::Session() {
  curl_ = std::unique_ptr<CurlHolder, std::function<void(CurlHolder *)>>(newHolder(), &freeHolder);
  auto curl = curl_->handle;
  if (curl) {
    // Set up some sensible defaults
    auto version_info = curl_version_info(CURLVERSION_NOW);
    auto version = std::string{"curl/"} + std::string{version_info->version};
    curl_easy_setopt(curl, CURLOPT_USERAGENT, version.data());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_->error);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

#if defined(_WIN32) || defined(__WIN32__) || defined(_MSC_VER)
    curl_easy_setopt(curl, CURLOPT_CAINFO, "curl-ca-bundle.crt");
#endif
#ifdef CPR_CURL_NOSIGNAL
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#endif
#if LIBCURL_VERSION_MAJOR >= 7
#if LIBCURL_VERSION_MINOR >= 25
#if LIBCURL_VERSION_PATCH >= 0
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
#endif
#endif
#endif
  }
}

void Session::freeHolder(CurlHolder *holder) {
  curl_easy_cleanup(holder->handle);
  curl_slist_free_all(holder->chunk);
  curl_formfree(holder->formpost);
  delete holder;
}

CurlHolder *Session::newHolder() {
  CurlHolder *holder = new CurlHolder();
  holder->handle = curl_easy_init();
  holder->chunk = NULL;
  holder->formpost = NULL;
  return holder;
}

void Session::applyDefaults(Defaults &defaults) {
  if (defaults.HasUrl()) {
    SetUrl(defaults.url_);
  }
  if (defaults.HasAuth()) {
    SetAuth(defaults.auth_);
  }
  if (defaults.HasTimeout()) {
    SetTimeout(defaults.timeout_);
  }
  SetHeaders(defaults.headers_);
  SetProtocolVersion(defaults.protocolVersion_);
  SetRetryPolicy(defaults.retryPolicy_);
}

void Session::PrepareHmac(curlinho::Hmac hmac, const std::string &path,
                          const std::string &method, const std::string &body) {
  hmac.prepareSignature(path, method, body);
  SetHeaders(hmac.getHmacHeaders());
}

void Session::SetUrl(const Url &url) {
  url_ = url;
}

void Session::AppendUrl(const Url &url) {
  url_ = url_ + url;
}

void Session::SetParameters(const Parameters &parameters) {
  parameters_ = parameters;
}

void Session::SetParameters(Parameters &&parameters) {
  parameters_ = std::move(parameters);
}

void Session::SetHeaders(const Headers &headers) {
  auto curl = curl_->handle;
  if (curl) {
    Headers newHeaders;
    newHeaders.insert(headers.begin(), headers.end());
    newHeaders.insert(headers_.begin(), headers_.end());
    headers_ = std::move(newHeaders);
    struct curl_slist *chunk = nullptr;
    for (const auto &header : headers_) {
      auto header_string = std::string{header.first};
       if (header.second.empty()) {
        header_string += ";";
      } else {
        header_string += ": " + header.second;
      }

      auto temp = curl_slist_append(chunk, header_string.data());
      if (temp) {
        chunk = temp;
      }
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

    curl_slist_free_all(curl_->chunk);
    curl_->chunk = chunk;
  }
}

void Session::SetTimeout(const Timeout &timeout) {
  auto curl = curl_->handle;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout.Milliseconds());
  }
}

void Session::SetAuth(const Authentication &auth) {
  auto curl = curl_->handle;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, auth.GetAuthString());
  }
}

void Session::SetBody(Body &&body) {
  auto curl = curl_->handle;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.data());
  }
}

void Session::SetBody(const Body &body) {
  auto curl = curl_->handle;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  }
}

void Session::SetProtocolVersion(const ProtocolVersion &protocol_version) {
#if LIBCURL_VERSION_MAJOR >= 7
#if LIBCURL_VERSION_MINOR >= 33
#if LIBCURL_VERSION_PATCH >= 0
  auto curl = curl_->handle;
  if (curl) {
    if (protocol_version == curlinho::HTTP::v2) {
      if (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2) {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
      } else {
        std::cerr << "No HTTP/2 support" << std::endl;
      }
    }
  }
#endif
#endif
#endif
}

void Session::SetRetryPolicy(const RetryPolicy &retryPolicy) {
  retryPolicy_ = retryPolicy;
}

Response Session::Get() {
  auto curl = curl_->handle;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
  }

  return makeRequestRetries(curl);
}

Response Session::Post() {
  auto curl = curl_->handle;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
  }

  return makeRequestRetries(curl);
}

Response Session::makeRequestRetries(CURL *curl) {
  auto r = makeRequest(curl);
  for (auto timer : retryPolicy_.delays_) {
    if (r.status_code != 200 || r.error.code != CURLE_OK) {
      if (r.error.code == CURLE_SSL_CACERT_BADFILE || r.error.code == CURLE_SSL_CERTPROBLEM) {
        CRL_LOG << "Certificate error, retry with no certificate!" << std::endl;
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
      } else if (r.error.code == CURLE_OPERATION_TIMEOUTED) {
        CRL_LOG << "Request timed out, Another retry!" << std::endl;
        CRL_SLEEP(timer);
      } else if (std::find(retryPolicy_.recoverableCodes_.begin(),
                           retryPolicy_.recoverableCodes_.end(),
                           r.status_code) == retryPolicy_.recoverableCodes_.end()) {
        CRL_LOG << "Error was not recoverable, no need to retry! - " << r.status_code << std::endl;
        break;
      } else {
        CRL_LOG << "Request failed, retry in " << timer << " seconds. - " << r.status_code
                << std::endl;
        CRL_SLEEP(timer);
      }
    } else {
      return r;
    }
    r = makeRequest(curl);
  }
  return r;
}

Response Session::makeRequest(CURL *curl) {
  if (!parameters_.content.empty()) {
    Url new_url{url_ + "?" + parameters_.content};
    curl_easy_setopt(curl, CURLOPT_URL, new_url.data());
  } else {
    curl_easy_setopt(curl, CURLOPT_URL, url_.data());
  }

  curl_->error[0] = '\0';

  std::string response_string;
  std::string header_string;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlinho::util::writeFunction);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_string);

  auto curl_error = curl_easy_perform(curl);

  char *raw_url;
  long response_code;
  double elapsed;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &elapsed);
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &raw_url);

  return Response{static_cast<std::int32_t>(response_code),
                  std::move(response_string),
                  curlinho::util::parseHeaders(header_string),
                  raw_url,
                  elapsed,
                  Error(curl_error, curl_->error)};
}

void Session::SetOption(const Url &url) {
  SetUrl(url);
}
void Session::SetOption(const Parameters &parameters) {
  SetParameters(parameters);
}
void Session::SetOption(Parameters &&parameters) {
  SetParameters(std::move(parameters));
}
void Session::SetOption(const Headers &header) {
  SetHeaders(header);
}
void Session::SetOption(const Timeout &timeout) {
  SetTimeout(timeout);
}
void Session::SetOption(const Authentication &auth) {
  SetAuth(auth);
}
void Session::SetOption(const Body &body) {
  SetBody(body);
}
void Session::SetOption(Body &&body) {
  SetBody(std::move(body));
}
void Session::SetOption(const ProtocolVersion &protocolVersion) {
  SetProtocolVersion(protocolVersion);
}
void Session::SetOption(const RetryPolicy &retryPolicy) {
  SetRetryPolicy(retryPolicy);
}

} // namespace curlinho
