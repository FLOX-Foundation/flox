/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/net/curl_transport.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace flox
{

namespace
{
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  auto* str = static_cast<std::string*>(userdata);
  str->append(ptr, size * nmemb);
  return size * nmemb;
}
}  // namespace

CurlTransport::CurlTransport(std::size_t poolSize, CurlTimeoutConfig timeoutConfig)
    : _pool(poolSize), _timeoutConfig(timeoutConfig)
{
  if (!_timeoutConfig.isValid())
  {
    throw std::invalid_argument("Invalid CurlTimeoutConfig");
  }
}

CurlTransport::CurlTransport(CurlSessionPoolConfig poolConfig, CurlTimeoutConfig timeoutConfig)
    : _pool(poolConfig), _timeoutConfig(timeoutConfig)
{
  if (!_timeoutConfig.isValid())
  {
    throw std::invalid_argument("Invalid CurlTimeoutConfig");
  }
}

CurlTransport::~CurlTransport() = default;

void CurlTransport::post(std::string_view url, std::string_view body,
                         const std::vector<std::pair<std::string_view, std::string_view>>& headers,
                         MoveOnlyFunction<void(std::string_view)> onSuccess,
                         MoveOnlyFunction<void(std::string_view)> onError)
{
  long connectSec = std::max(1L, static_cast<long>(_timeoutConfig.connectTimeoutMs / 1000));
  long requestSec = std::max(1L, static_cast<long>(_timeoutConfig.requestTimeoutMs / 1000));

  postImpl(url, body, headers, std::move(onSuccess), std::move(onError), connectSec, requestSec);
}

void CurlTransport::postWithTimeout(
    std::string_view url, std::string_view body,
    const std::vector<std::pair<std::string_view, std::string_view>>& headers,
    MoveOnlyFunction<void(std::string_view)> onSuccess,
    MoveOnlyFunction<void(std::string_view)> onError, int requestTimeoutMs)
{
  long connectSec = std::max(1L, static_cast<long>(_timeoutConfig.connectTimeoutMs / 1000));
  long requestSec = std::max(1L, static_cast<long>(requestTimeoutMs / 1000));

  postImpl(url, body, headers, std::move(onSuccess), std::move(onError), connectSec, requestSec);
}

void CurlTransport::postImpl(
    std::string_view url, std::string_view body,
    const std::vector<std::pair<std::string_view, std::string_view>>& headers,
    MoveOnlyFunction<void(std::string_view)> onSuccess,
    MoveOnlyFunction<void(std::string_view)> onError, long connectTimeoutSec,
    long requestTimeoutSec)
{
  CURL* h = _pool.acquire();
  if (!h)
  {
    if (onError)
    {
      onError("Connection pool exhausted or timeout");
    }
    return;
  }
  curl_easy_reset(h);

  // Copy string_views to ensure lifetime
  std::string urlStr(url);
  std::string bodyStr(body);

  curl_easy_setopt(h, CURLOPT_URL, urlStr.c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, bodyStr.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));

  // Configurable timeouts
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, connectTimeoutSec);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, requestTimeoutSec);

  // Connection reuse
  curl_easy_setopt(h, CURLOPT_FORBID_REUSE, 0L);
  curl_easy_setopt(h, CURLOPT_FRESH_CONNECT, 0L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPIDLE, 30L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPINTVL, 15L);

  curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

  std::string response;
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &response);

  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Connection: keep-alive");
  for (const auto& [k, v] : headers)
  {
    std::string hv;
    hv.reserve(k.size() + v.size() + 3);
    hv.append(k);
    hv.append(": ");
    hv.append(v);
    hdrs = curl_slist_append(hdrs, hv.c_str());
  }
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

  CURLcode res = curl_easy_perform(h);

  long httpCode = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_slist_free_all(hdrs);
  _pool.release(h);

  if (res == CURLE_OK)
  {
    if (httpCode >= 200 && httpCode < 300)
    {
      if (onSuccess)
      {
        onSuccess(response);
      }
    }
    else
    {
      if (onError)
      {
        std::string errMsg = "HTTP " + std::to_string(httpCode);
        if (!response.empty())
        {
          if (response.size() > 1024)
          {
            errMsg += ": " + response.substr(0, 1024) + "...";
          }
          else
          {
            errMsg += ": " + response;
          }
        }
        onError(errMsg);
      }
    }
  }
  else
  {
    if (onError)
    {
      onError(curl_easy_strerror(res));
    }
  }
}

}  // namespace flox
