/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/net/ix_websocket_client.h"

#include <string>

namespace flox
{

IxWebSocketClient::IxWebSocketClient(std::string url, std::string origin, int reconnectDelayMs,
                                     ILogger* logger, int pingIntervalSec, std::string userAgent)
    : _url(std::move(url)),
      _origin(std::move(origin)),
      _reconnectDelayMs(reconnectDelayMs),
      _pingIntervalSec(pingIntervalSec),
      _userAgent(std::move(userAgent)),
      _logger(logger),
      _ws(std::make_unique<ix::WebSocket>())
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
  if (!_running.exchange(false))
  {
    return;
  }
  // Deliberately does not touch _ws. The run() thread owns the socket: it
  // replaces it under _sendMutex on every reconnect attempt and stops it when
  // its wait loop observes _running == false. Stopping it from here as well
  // raced that ownership two ways at once: two threads joining ix's worker
  // thread (the loser gets EINVAL and ix throws -- fatally, when the loser is
  // the run() thread, because the exception leaves the thread body), and an
  // unsynchronized read of _ws while run() swaps in a fresh socket. The cost
  // of the single-owner rule is shutdown latency of one wait-loop tick.
}

void IxWebSocketClient::send(const std::string& data)
{
  std::lock_guard lock(_sendMutex);
  _ws->send(data);
}

void IxWebSocketClient::run()
{
  constexpr int MAX_BACKOFF_MS = 30000;

  while (_running)
  {
    // Fresh socket every attempt — avoids stale TLS/frame state
    {
      std::lock_guard lock(_sendMutex);
      _ws = std::make_unique<ix::WebSocket>();
    }

    _ws->disableAutomaticReconnection();
    _ws->setUrl(_url);
    if (!_origin.empty())
    {
      if (_userAgent.empty())
      {
        _ws->setExtraHeaders({{"Origin", _origin}});
      }
      else
      {
        _ws->setExtraHeaders({{"Origin", _origin}, {"User-Agent", _userAgent}});
      }
    }
    else if (!_userAgent.empty())
    {
      _ws->setExtraHeaders({{"User-Agent", _userAgent}});
    }
    _ws->disablePerMessageDeflate();
    if (_pingIntervalSec > 0)
    {
      _ws->setPingInterval(_pingIntervalSec);
    }
    else
    {
      _ws->setPingInterval(-1);
    }

    // Local flag — only THIS iteration's callbacks can set it.
    // Eliminates race between old socket callbacks and new socket state.
    std::atomic<bool> connectionClosed{false};

    _ws->setOnMessageCallback(
        [this, &connectionClosed](const ix::WebSocketMessagePtr& msg)
        {
          switch (msg->type)
          {
            case ix::WebSocketMessageType::Open:
              _logger->info("WebSocket connected to " + _url);
              _consecutiveFailures = 0;
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
              connectionClosed.store(true, std::memory_order_release);
              break;

            case ix::WebSocketMessageType::Error:
              _logger->error("WebSocket error connecting to " + _url + ": " +
                             msg->errorInfo.reason);
              connectionClosed.store(true, std::memory_order_release);
              break;

            default:
              break;
          }
        });

    _ws->start();

    // Wait for close/error callback or stop signal — NOT polling getReadyState()
    while (_running && !connectionClosed.load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Ensure socket is fully stopped before creating a new one. Sole owner of
    // this call -- see stop(). The catch is belt-and-braces: an exception
    // escaping a thread body is std::terminate, and no teardown problem on one
    // socket is worth the process.
    try
    {
      _ws->stop();
    }
    catch (const std::exception& e)
    {
      _logger->error("WebSocket stop threw (continuing): " + std::string(e.what()));
    }

    if (_running)
    {
      ++_consecutiveFailures;
      int backoffMs =
          std::min(_reconnectDelayMs * (1 << std::min(_consecutiveFailures, 4)), MAX_BACKOFF_MS);
      _logger->warn("WebSocket disconnected, retrying in " + std::to_string(backoffMs) +
                    "ms... (attempt " + std::to_string(_consecutiveFailures) + ")");
      std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
    }
  }
}

}  // namespace flox
