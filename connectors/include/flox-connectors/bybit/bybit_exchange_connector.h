/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox-connectors/net/abstract_websocket_client.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/common.h"
#include "flox/connector/abstract_exchange_connector.h"
#include "flox/log/abstract_logger.h"

#include <simdjson/simdjson.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace flox
{

struct BybitConfig
{
  std::string endpoint;
  std::vector<std::string> symbols;
  int reconnectDelayMs{2000};
};

class BybitExchangeConnector : public IExchangeConnector
{
 public:
  BybitExchangeConnector(
      const BybitConfig& config,
      BookUpdateBus* bookUpdateBus,
      TradeBus* tradeBus,
      std::move_only_function<SymbolId(std::string_view)> symbolMapper,
      std::shared_ptr<ILogger> logger);

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "bybit"; }

 private:
  void handleMessage(std::string_view payload);

  std::vector<std::string> _symbols;
  std::string _endpoint;

  BookUpdateBus* _bookUpdateBus;
  TradeBus* _tradeBus;

  std::move_only_function<SymbolId(std::string_view)> _getSymbolId;
  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::atomic<bool> _running{false};

  pool::Pool<BookUpdateEvent, 2047> _bookPool;
};

}  // namespace flox
