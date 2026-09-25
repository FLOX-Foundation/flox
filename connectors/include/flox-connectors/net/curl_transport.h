/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox-connectors/net/curl_session_pool.h"

#include <flox/net/abstract_transport.h>
#include <flox/util/base/move_only_function.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace flox
{

struct CurlTimeoutConfig
{
  int connectTimeoutMs{10000};  // 10s default
  int requestTimeoutMs{30000};  // 30s default

  bool isValid() const { return connectTimeoutMs > 0 && requestTimeoutMs > 0; }
};

// How the transport gets the request onto the wire.
//
// Threading model: post() copies the request, hands it to a sender thread and
// returns; curl_easy_perform and both completion callbacks run on that thread,
// never on the caller's. The order path calls post() from the strategy /
// event-bus consumer thread, and a venue that accepts the connection and then
// says nothing is indistinguishable from a healthy one until the request
// timeout fires -- performing the request inline stopped that thread from
// processing anything, market data included, for the whole round trip (30 s
// with the default configuration). The outcome reaches the strategy the same
// way it always did, as an OrderEvent on the OrderExecutionBus published from
// the callback.
//
// One sender thread by default, so requests leave in the order they were
// handed over: a cancel submitted after a place is sent after it. Raising the
// count raises throughput against a slow venue and gives that ordering up.
//
// The handoff is bounded. A full queue means the venue is not draining
// requests as fast as the strategy produces them; the request is refused
// through onError rather than growing the queue without limit, so the caller
// publishes a rejection instead of believing an order is in flight.
struct CurlDispatchConfig
{
  std::size_t senderThreads{1};
  std::size_t maxQueueDepth{1024};

  bool isValid() const { return senderThreads > 0 && maxQueueDepth > 0; }
};

class CurlTransport : public ITransport
{
 public:
  explicit CurlTransport(std::size_t poolSize = 4, CurlTimeoutConfig timeoutConfig = {},
                         CurlDispatchConfig dispatchConfig = {});
  explicit CurlTransport(CurlSessionPoolConfig poolConfig, CurlTimeoutConfig timeoutConfig = {},
                         CurlDispatchConfig dispatchConfig = {});
  ~CurlTransport() override;

  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>& headers,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)> onError) override;

  void postWithTimeout(std::string_view url, std::string_view body,
                       const std::vector<std::pair<std::string_view, std::string_view>>& headers,
                       MoveOnlyFunction<void(std::string_view)> onSuccess,
                       MoveOnlyFunction<void(std::string_view)> onError, int requestTimeoutMs);

  // Joins the sender threads and answers everything still queued through
  // onError. Called by the destructor too; safe to call twice.
  void stop() override;

 private:
  // Owns every byte it needs: the caller's string_views are gone by the time
  // the sender thread picks the request up.
  struct Request
  {
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    MoveOnlyFunction<void(std::string_view)> onSuccess;
    MoveOnlyFunction<void(std::string_view)> onError;
    long connectTimeoutMs{0};
    long requestTimeoutMs{0};
  };

  void submit(std::string_view url, std::string_view body,
              const std::vector<std::pair<std::string_view, std::string_view>>& headers,
              MoveOnlyFunction<void(std::string_view)> onSuccess,
              MoveOnlyFunction<void(std::string_view)> onError, long connectTimeoutMs,
              long requestTimeoutMs);

  void startSenders();
  void senderLoop();
  void perform(Request& req);

  CurlSessionPool _pool;
  CurlTimeoutConfig _timeoutConfig;
  CurlDispatchConfig _dispatchConfig;

  std::mutex _queueMutex;
  std::condition_variable _queueCv;
  std::deque<Request> _queue;
  bool _stopping{false};
  std::vector<std::thread> _senders;
};

}  // namespace flox
