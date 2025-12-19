/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <flox/log/log.h>

#include <string>

#include "flox-connectors/net/ix_websocket_client.h"

namespace flox
{

IxWebSocketClient::IxWebSocketClient(std::string url, std::string origin, int reconnectDelayMs,
                                     ILogger* logger, int pingIntervalSec)
    : _url(std::move(url)),
      _origin(std::move(origin)),
      _reconnectDelayMs(reconnectDelayMs),
      _pingIntervalSec(pingIntervalSec),
      _logger(logger)
{
}

IxWebSocketClient::~IxWebSocketClient()
{
  stop();

  if (_thread.joinable())
  {
    _thread.join();
  }
}

void IxWebSocketClient::onOpen(MoveOnlyFunction<void()> cb) { _onOpen = std::move(cb); }

void IxWebSocketClient::onMessage(MoveOnlyFunction<void(std::string_view)> cb)
{
  _onMessage = std::move(cb);
}

void IxWebSocketClient::onClose(MoveOnlyFunction<void(int, std::string_view)> cb)
{
  _onClose = std::move(cb);
}

void IxWebSocketClient::start()
{
  if (_running.exchange(true))
  {
    return;
  }

  _thread = std::thread(&IxWebSocketClient::run, this);
}

void IxWebSocketClient::stop()
{
  _running = false;
  _ws.stop();
}

void IxWebSocketClient::send(const std::string& data)
{
  std::lock_guard lock(_sendMutex);
  _ws.send(data);
}

void IxWebSocketClient::run()
{
  while (_running)
  {
    _ws.setUrl(_url);
    _ws.setExtraHeaders({{"Origin", _origin}});
    _ws.disablePerMessageDeflate();
    if (_pingIntervalSec > 0)
    {
      _ws.setPingInterval(_pingIntervalSec);
    }
    else
    {
      // Some servers require application-level heartbeats
      _ws.setPingInterval(-1);
    }

    _ws.setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& msg)
        {
          switch (msg->type)
          {
            case ix::WebSocketMessageType::Open:
              if (_onOpen)
              {
                _onOpen();
              }

              break;
            case ix::WebSocketMessageType::Message:
              if (_onMessage)
              {
                _onMessage(msg->str);
              }
              break;

            case ix::WebSocketMessageType::Close:
              if (_onClose)
              {
                _onClose(msg->closeInfo.code, msg->closeInfo.reason);
              }
              break;
            case ix::WebSocketMessageType::Error:
              FLOX_LOG_ERROR("WebSocket error: " << msg->errorInfo.reason);
              _logger->warn("WebSocket error: " + msg->errorInfo.reason);
              break;

            default:
              break;
          }
        });

    _ws.start();

    while (_ws.getReadyState() == ix::ReadyState::Open && _running)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    FLOX_LOG_WARN("WebSocket disconnected, retrying in " << std::to_string(_reconnectDelayMs)
                                                         << "ms...");
    _logger->warn("WebSocket disconnected, retrying in " + std::to_string(_reconnectDelayMs) +
                  "ms...");
    std::this_thread::sleep_for(std::chrono::milliseconds(_reconnectDelayMs));
  }
}

}  // namespace flox