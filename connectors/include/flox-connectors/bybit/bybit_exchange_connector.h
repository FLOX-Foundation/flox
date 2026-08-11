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
#include <vector>

namespace flox
{

struct BybitConfig
{
  enum class BookDepth
  {
    Invalid = -1,
    Top1 = 1,
    Top25 = 25,
    Top50 = 50,
    Top100 = 100,
    Top200 = 200,
    Top500 = 500
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
  bool enablePrivate = false;
};

class BybitExchangeConnector : public IExchangeConnector
{
 public:
  BybitExchangeConnector(const BybitConfig& config, BookUpdateBus* bookUpdateBus,
                         TradeBus* tradeBus, OrderExecutionBus* orderBus, SymbolRegistry* registry,
                         std::shared_ptr<ILogger> logger);

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "bybit"; }

  SymbolId resolveSymbolId(std::string_view symbol);

  // Public so protocol behaviour (sequencing, gap resync) is testable offline
  // by feeding raw frames without a live socket.
  void handleMessage(std::string_view payload);

  // Book deltas whose update id broke continuity (dropped + resync triggered).
  uint64_t bookGapCount() const noexcept { return _bookGapCount.load(std::memory_order_relaxed); }

 private:
  void handlePrivateMessage(std::string_view payload);

  // Re-subscribe one symbol's orderbook topic so the exchange re-sends a
  // snapshot (gap recovery). No-op before start() (no socket yet).
  void resubscribeBook(std::string_view symbolName);

  BybitConfig _config;

  BookUpdateBus* _bookUpdateBus;
  TradeBus* _tradeBus;

  SymbolRegistry* _registry = nullptr;

  // Per-symbol book continuity: Bybit v5 orderbook deltas carry an update id
  // ("u") that increments by 1 per message; a jump means a dropped frame and a
  // silently wrong book, so the connector drops the delta and re-subscribes for
  // a fresh snapshot. resyncInFlight suppresses repeat resubscribes while the
  // snapshot is on its way.
  struct BookSeqState
  {
    int64_t lastUpdateId{-1};
    bool resyncInFlight{false};
  };
  std::unordered_map<SymbolId, BookSeqState> _bookSeq;
  std::atomic<uint64_t> _bookGapCount{0};

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::unique_ptr<IWebSocketClient> _wsClientPrivate;
  std::atomic<bool> _running{false};

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;
  OrderExecutionBus* _orderBus = nullptr;
  std::thread _pingThread;
};

}  // namespace flox
