/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <flox/log/abstract_logger.h>
#include <flox/net/abstract_websocket_client.h>
#include <flox/util/base/move_only_function.h>

namespace flox
{

class IxWebSocketClient : public IWebSocketClient
{
 public:
  IxWebSocketClient(std::string url, std::string origin, int reconnectDelayMs, ILogger* logger,
                    int pingIntervalSec = 1, std::string userAgent = "");
  ~IxWebSocketClient() override;

  void onOpen(MoveOnlyFunction<void()> cb) override;
  void onMessage(MoveOnlyFunction<void(std::string_view)> cb) override;
  void onClose(MoveOnlyFunction<void(int, std::string_view)> cb) override;

  // Returns whether the frame was actually handed to the socket
  // (ix::WebSocket::send()'s own success flag). false on a closed/not-yet-open
  // socket -- callers that need the frame delivered (e.g. gap resync) must
  // check it instead of assuming the write always lands.
  bool send(const std::string& data) override;
  void start() override;
  void stop() override;

 private:
  void run();

  std::string _url;
  std::string _origin;
  int _reconnectDelayMs;
  int _pingIntervalSec;
  std::string _userAgent;
  ILogger* _logger;

  std::atomic<bool> _running{false};
  std::unique_ptr<ix::WebSocket> _ws;
  std::thread _thread;
  std::mutex _sendMutex;

  // Guards the reconnect backoff wait so stop() can interrupt it instead of
  // sleeping it out (an uninterruptible sleep_for here used to make
  // shutdown take up to MAX_BACKOFF_MS instead of one wait-loop tick).
  std::mutex _backoffMutex;
  std::condition_variable _backoffCv;

  MoveOnlyFunction<void()> _onOpen;
  MoveOnlyFunction<void(std::string_view)> _onMessage;
  MoveOnlyFunction<void(int, std::string_view)> _onClose;

  // Written from the ix callback thread (reset to 0 on Open), read and
  // incremented from run(). Plain int next to a deliberately atomic _running
  // was flagged as a hygiene risk: the only path that could race it
  // requires ix::WebSocket::stop() to throw and leave its worker thread
  // alive, which was not reproduced, but atomic costs nothing here.
  std::atomic<int> _consecutiveFailures{0};
};

}  // namespace flox