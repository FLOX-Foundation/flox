/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/net/curl_transport.h"

#include <flox/log/log.h>

#include <algorithm>
#include <exception>
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

// postImpl runs onSuccess/onError in the calling thread, synchronously,
// inside curl_easy_perform's stack frame. Every caller in this codebase
// parses the response with simdjson in "exceptions on" mode (the default),
// and none of the five REST callbacks across the connectors guarded against
// that -- a malformed or unexpected response (e.g. HTTP 200 with a body that
// has no "retCode") threw out of postImpl into whoever called post(). On the
// normal per-event path that is the event-bus consumer thread: EventBus's
// run loop catches around EventDispatcher::dispatch, marks that consumer
// dead, and (for a REQUIRED consumer) stalls gating there for checkHealth()
// to surface -- no process-wide abort on that path. The drain-on-stop path
// is different: its dispatch call is not wrapped the same way, so an
// exception there is not this connector's to catch, only to avoid causing.
// One guard here protects every current and future caller instead of
// wrapping each callback individually.
void invokeSafely(MoveOnlyFunction<void(std::string_view)>& fn, std::string_view arg,
                  const char* which)
{
  if (!fn)
  {
    return;
  }
  try
  {
    fn(arg);
  }
  catch (const std::exception& e)
  {
    FLOX_LOG_ERROR("[CurlTransport] " << which << " handler threw: " << e.what());
  }
  catch (...)
  {
    FLOX_LOG_ERROR("[CurlTransport] " << which << " handler threw a non-standard exception");
  }
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
    invokeSafely(onError, "Connection pool exhausted or timeout", "onError");
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
      invokeSafely(onSuccess, response, "onSuccess");
    }
    else
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
      invokeSafely(onError, errMsg, "onError");
    }
  }
  else
  {
    invokeSafely(onError, curl_easy_strerror(res), "onError");
  }
}

}  // namespace flox
