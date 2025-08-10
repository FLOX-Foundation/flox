/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/connector/abstract_exchange_connector.h>
#include <flox/engine/symbol_registry.h>
#include <flox/log/abstract_logger.h>
#include <flox/net/abstract_websocket_client.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace flox
{

struct HyperliquidConfig
{
  bool isValid() const { return !wsEndpoint.empty() && !restEndpoint.empty(); }

  std::string wsEndpoint{"wss://api.hyperliquid.xyz/ws"};
  std::string restEndpoint{"https://api.hyperliquid.xyz/exchange"};
  std::vector<std::string> symbols;
  std::string privateKey;
  int reconnectDelayMs{2000};
};

class HyperliquidExchangeConnector : public IExchangeConnector
{
 public:
  HyperliquidExchangeConnector(const HyperliquidConfig& config, BookUpdateBus* bookBus,
                               TradeBus* tradeBus, SymbolRegistry* symbolRegistry,
                               std::shared_ptr<ILogger> logger);

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "hyperliquid"; }

  SymbolId resolveSymbolId(std::string_view symbol);

 private:
  void handleMessage(std::string_view payload);

  HyperliquidConfig _config;

  BookUpdateBus* _bookBus;
  TradeBus* _tradeBus;
  SymbolRegistry* _registry{nullptr};

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::atomic<bool> _running{false};

  pool::Pool<BookUpdateEvent, 2047> _bookPool;
};

}  // namespace flox
