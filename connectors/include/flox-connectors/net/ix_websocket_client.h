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
  IxWebSocketClient(std::string url, std::string origin, int reconnectDelayMs, ILogger* logger);
  ~IxWebSocketClient() override;

  void onOpen(MoveOnlyFunction<void()> cb) override;
  void onMessage(MoveOnlyFunction<void(std::string_view)> cb) override;
  void onClose(MoveOnlyFunction<void(int, std::string_view)> cb) override;

  void send(const std::string& data) override;
  void start() override;
  void stop() override;

 private:
  void run();

  std::string _url;
  std::string _origin;
  int _reconnectDelayMs;
  ILogger* _logger;

  std::atomic<bool> _running{false};
  ix::WebSocket _ws;
  std::thread _thread;
  std::mutex _sendMutex;

  MoveOnlyFunction<void()> _onOpen;
  MoveOnlyFunction<void(std::string_view)> _onMessage;
  MoveOnlyFunction<void(int, std::string_view)> _onClose;
};

}  // namespace flox