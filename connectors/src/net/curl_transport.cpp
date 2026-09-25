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

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

// The completion callbacks run on a sender thread, inside curl_easy_perform's
// stack frame. Every caller in this codebase parses the response with simdjson
// in "exceptions on" mode (the default), and none of the five REST callbacks
// across the connectors guarded against that -- a malformed or unexpected
// response (e.g. HTTP 200 with a body that has no "retCode") threw out of the
// callback. On a sender thread an escaping exception is std::terminate, so the
// guard matters more here than it did when the callback ran on the caller's
// stack. One guard protects every current and future caller instead of
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

CurlTransport::CurlTransport(std::size_t poolSize, CurlTimeoutConfig timeoutConfig,
                             CurlDispatchConfig dispatchConfig)
    : _pool(poolSize), _timeoutConfig(timeoutConfig), _dispatchConfig(dispatchConfig)
{
  if (!_timeoutConfig.isValid())
  {
    throw std::invalid_argument("Invalid CurlTimeoutConfig");
  }
  if (!_dispatchConfig.isValid())
  {
    throw std::invalid_argument("Invalid CurlDispatchConfig");
  }
  startSenders();
}

CurlTransport::CurlTransport(CurlSessionPoolConfig poolConfig, CurlTimeoutConfig timeoutConfig,
                             CurlDispatchConfig dispatchConfig)
    : _pool(poolConfig), _timeoutConfig(timeoutConfig), _dispatchConfig(dispatchConfig)
{
  if (!_timeoutConfig.isValid())
  {
    throw std::invalid_argument("Invalid CurlTimeoutConfig");
  }
  if (!_dispatchConfig.isValid())
  {
    throw std::invalid_argument("Invalid CurlDispatchConfig");
  }
  startSenders();
}

CurlTransport::~CurlTransport() { stop(); }

void CurlTransport::startSenders()
{
  _senders.reserve(_dispatchConfig.senderThreads);
  for (std::size_t i = 0; i < _dispatchConfig.senderThreads; ++i)
  {
    _senders.emplace_back(
        [this]
        {
          senderLoop();
        });
  }
}

void CurlTransport::stop()
{
  {
    std::lock_guard<std::mutex> lk(_queueMutex);
    _stopping = true;
  }
  _queueCv.notify_all();

  for (auto& t : _senders)
  {
    if (t.joinable())
    {
      t.join();
    }
  }
  _senders.clear();

  // Whatever was still queued never reached the venue, and a caller waiting
  // on an outcome has to be told so rather than left with an order it
  // believes is in flight.
  std::deque<Request> leftovers;
  {
    std::lock_guard<std::mutex> lk(_queueMutex);
    leftovers.swap(_queue);
  }
  for (auto& req : leftovers)
  {
    invokeSafely(req.onError, "Transport stopped before the request was sent", "onError");
  }
}

void CurlTransport::post(std::string_view url, std::string_view body,
                         const std::vector<std::pair<std::string_view, std::string_view>>& headers,
                         MoveOnlyFunction<void(std::string_view)> onSuccess,
                         MoveOnlyFunction<void(std::string_view)> onError)
{
  submit(url, body, headers, std::move(onSuccess), std::move(onError),
         _timeoutConfig.connectTimeoutMs, _timeoutConfig.requestTimeoutMs);
}

void CurlTransport::postWithTimeout(
    std::string_view url, std::string_view body,
    const std::vector<std::pair<std::string_view, std::string_view>>& headers,
    MoveOnlyFunction<void(std::string_view)> onSuccess,
    MoveOnlyFunction<void(std::string_view)> onError, int requestTimeoutMs)
{
  submit(url, body, headers, std::move(onSuccess), std::move(onError),
         _timeoutConfig.connectTimeoutMs, requestTimeoutMs);
}

void CurlTransport::submit(
    std::string_view url, std::string_view body,
    const std::vector<std::pair<std::string_view, std::string_view>>& headers,
    MoveOnlyFunction<void(std::string_view)> onSuccess,
    MoveOnlyFunction<void(std::string_view)> onError, long connectTimeoutMs, long requestTimeoutMs)
{
  Request req;
  req.url.assign(url);
  req.body.assign(body);
  req.headers.reserve(headers.size());
  for (const auto& [k, v] : headers)
  {
    req.headers.emplace_back(std::string(k), std::string(v));
  }
  req.onSuccess = std::move(onSuccess);
  req.onError = std::move(onError);
  req.connectTimeoutMs = connectTimeoutMs;
  req.requestTimeoutMs = requestTimeoutMs;

  {
    std::unique_lock<std::mutex> lk(_queueMutex);
    if (_stopping)
    {
      lk.unlock();
      invokeSafely(req.onError, "Transport is stopped", "onError");
      return;
    }
    if (_queue.size() >= _dispatchConfig.maxQueueDepth)
    {
      lk.unlock();
      FLOX_LOG_ERROR("[CurlTransport] send queue full (" << _dispatchConfig.maxQueueDepth
                                                         << "), refusing request to " << req.url);
      invokeSafely(req.onError, "Transport send queue is full", "onError");
      return;
    }
    _queue.push_back(std::move(req));
  }
  _queueCv.notify_one();
}

void CurlTransport::senderLoop()
{
  for (;;)
  {
    Request req;
    {
      std::unique_lock<std::mutex> lk(_queueMutex);
      _queueCv.wait(lk,
                    [this]
                    {
                      return _stopping || !_queue.empty();
                    });
      if (_stopping)
      {
        return;
      }
      req = std::move(_queue.front());
      _queue.pop_front();
    }

    perform(req);
  }
}

void CurlTransport::perform(Request& req)
{
  CURL* h = _pool.acquire();
  if (!h)
  {
    invokeSafely(req.onError, "Connection pool exhausted or timeout", "onError");
    return;
  }
  curl_easy_reset(h);

  curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));

  // Millisecond timeouts, as the configuration and postWithTimeout() have
  // always claimed to take: the second-resolution options truncated 1500 ms to
  // one second and raised 250 ms to the same, which is the whole resolution
  // the order path needs. NOSIGNAL goes with them -- libcurl's signal-based
  // timeout path has one-second granularity and is not safe to use from
  // several threads.
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, req.connectTimeoutMs);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, req.requestTimeoutMs);

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
  for (const auto& [k, v] : req.headers)
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
      invokeSafely(req.onSuccess, response, "onSuccess");
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
      invokeSafely(req.onError, errMsg, "onError");
    }
  }
  else
  {
    invokeSafely(req.onError, curl_easy_strerror(res), "onError");
  }
}

}  // namespace flox
