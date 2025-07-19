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

#include "flox-connectors/net/abstract_websocket_client.h"
#include "flox/log/abstract_logger.h"

namespace flox
{

class IxWebSocketClient : public IWebSocketClient
{
 public:
  IxWebSocketClient(std::string url, std::string origin, int reconnectDelayMs, ILogger* logger);
  ~IxWebSocketClient() override;

  void onOpen(std::move_only_function<void()> cb) override;
  void onMessage(std::move_only_function<void(std::string_view)> cb) override;
  void onClose(std::move_only_function<void(int, std::string_view)> cb) override;

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

  std::move_only_function<void()> _onOpen;
  std::move_only_function<void(std::string_view)> _onMessage;
  std::move_only_function<void(int, std::string_view)> _onClose;
};

}  // namespace flox