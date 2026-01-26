/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "polymarket_config.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/connector/abstract_exchange_connector.h>
#include <flox/engine/symbol_registry.h>
#include <flox/log/abstract_logger.h>
#include <flox/net/abstract_websocket_client.h>

#include <simdjson.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace flox
{

class PolymarketExchangeConnector : public IExchangeConnector
{
 public:
  PolymarketExchangeConnector(const PolymarketConfig& config, BookUpdateBus* bookUpdateBus,
                              TradeBus* tradeBus, SymbolRegistry* registry,
                              std::shared_ptr<ILogger> logger);

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "polymarket"; }

  SymbolId resolveSymbolId(std::string_view tokenId);

 private:
  void handleMessage(std::string_view payload);
  void processBookSnapshot(simdjson::ondemand::object obj, uint64_t recvNs);
  void sendSubscribe(const std::vector<std::string>& tokenIds, const std::string& operation);

  PolymarketConfig _config;

  BookUpdateBus* _bookUpdateBus;
  TradeBus* _tradeBus;
  SymbolRegistry* _registry = nullptr;

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsMarket;
  std::atomic<bool> _running{false};

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;

  std::unordered_map<std::string, SymbolId> _tokenToSymbol;
};

}  // namespace flox
