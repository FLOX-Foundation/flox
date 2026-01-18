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
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/log/abstract_logger.h>
#include <flox/net/abstract_websocket_client.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace flox
{

struct BitgetConfig
{
  enum class BookDepth
  {
    Invalid = -1,
    Depth1 = 1,
    Depth5 = 5,
    Depth15 = 15,
    DepthFull = 100  // "books" channel - all levels
  };

  struct SymbolEntry
  {
    std::string name;
    InstrumentType type;
    BookDepth depth = BookDepth::Invalid;
  };

  bool isValid() const;

  std::string publicEndpoint;
  std::string privateEndpoint;
  std::vector<SymbolEntry> symbols;
  int reconnectDelayMs{2000};
  std::string apiKey;
  std::string apiSecret;
  std::string passphrase;
  bool enablePrivate = false;
};

class BitgetExchangeConnector : public IExchangeConnector
{
 public:
  BitgetExchangeConnector(const BitgetConfig& config, BookUpdateBus* bookUpdateBus,
                          TradeBus* tradeBus, OrderExecutionBus* orderBus, SymbolRegistry* registry,
                          std::shared_ptr<ILogger> logger);

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "bitget"; }

  SymbolId resolveSymbolId(std::string_view symbol);

 private:
  void handleMessage(std::string_view payload);
  void handlePrivateMessage(std::string_view payload);
  void pingLoop();

  BitgetConfig _config;

  BookUpdateBus* _bookUpdateBus;
  TradeBus* _tradeBus;

  SymbolRegistry* _registry = nullptr;

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::unique_ptr<IWebSocketClient> _wsClientPrivate;
  std::atomic<bool> _running{false};
  std::thread _pingThread;

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;
  OrderExecutionBus* _orderBus = nullptr;
};

}  // namespace flox
