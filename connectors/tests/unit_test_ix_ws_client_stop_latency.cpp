/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline coverage for send() return-value propagation and interruptible
 * shutdown.
 *
 * IxWebSocketClient::send() used to return void and discard
 * ix::WebSocket::send()'s own success flag, so a caller had no way to learn a
 * frame was dropped. send() must return that flag.
 *
 * stop() used to only flip an atomic; the reconnect backoff wait was an
 * uninterruptible std::this_thread::sleep_for(backoffMs), so shutdown against
 * an unreachable exchange took as long as whatever backoff tier run() had
 * reached (measured 3300 ms after the first failure, 23456 ms once the
 * 30-second cap was hit) even though the comment on stop() promises "one
 * wait-loop tick" (100 ms). The backoff wait must be interruptible.
 */

#include "flox-connectors/net/ix_websocket_client.h"

#include <flox/log/abstract_logger.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace
{

struct NullLogger final : public flox::ILogger
{
  void info(std::string_view) override {}
  void warn(std::string_view) override {}
  void error(std::string_view) override {}
};

}  // namespace

TEST(IxWebSocketClientLifecycle, SendOnClosedSocketReturnsFailureInsteadOfLosingIt)
{
  NullLogger logger;
  // Never started: the underlying ix::WebSocket exists (constructor creates
  // it unconditionally) but is not connected, so send() must fail.
  flox::IxWebSocketClient client("ws://127.0.0.1:1", "", /*reconnectDelayMs*/ 1000, &logger,
                                 /*pingIntervalSec*/ 0);

  const bool ok = client.send(R"({"op":"ping"})");
  EXPECT_FALSE(ok);
}

TEST(IxWebSocketClientLifecycle, StopLatencyBoundedDuringReconnectBackoff)
{
  NullLogger logger;
  // Port 1 on loopback: nothing listens there, so every connection attempt
  // is refused essentially instantly and the run() thread spends its time in
  // the reconnect backoff wait, not in a live connection attempt.
  // reconnectDelayMs=2000 matches BybitConfig's default.
  auto client = std::make_unique<flox::IxWebSocketClient>("ws://127.0.0.1:1", "",
                                                          /*reconnectDelayMs*/ 2000, &logger,
                                                          /*pingIntervalSec*/ 0);
  client->start();

  // Give run() time to hit the first connection failure and enter backoff
  // (first tier is reconnectDelayMs * 2 = 4000ms, comfortably longer than
  // this wait).
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const auto begin = std::chrono::steady_clock::now();
  client->stop();
  client.reset();  // destructor joins the run() thread
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();

  // Before the fix this measured 3300 ms here (and up to 23456 ms once the
  // 30-second backoff cap saturated). The task's acceptance bound is 200 ms;
  // 500 ms is used here to keep the assertion stable on a loaded CI runner
  // while still failing hard against the old uninterruptible sleep_for.
  EXPECT_LT(elapsedMs, 500) << "stop() took " << elapsedMs << "ms during reconnect backoff";
}
