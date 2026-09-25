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
#include <thread>
#include <vector>

namespace flox
{

struct HyperliquidConfig
{
  // The connector is WS-only; it never reads restEndpoint, so validity gates
  // on the WS endpoint alone. Order signing credentials live on the executor
  // (passed to its ctor), not here -- the config no longer carries a dead
  // privateKey field.
  bool isValid() const { return !wsEndpoint.empty(); }

  std::string wsEndpoint{"wss://api.hyperliquid.xyz/ws"};
  std::string restEndpoint{"https://api.hyperliquid.xyz/exchange"};
  std::vector<std::string> symbols;
  int reconnectDelayMs{2000};
  // Window after which a symbol that stopped ticking is reported through
  // emitStaleData. 0 disables the check: the right window is a property of
  // the instrument's liquidity, not of the venue, so there is no default the
  // connector can pick for you.
  int staleDataTimeoutMs{0};
};

class HyperliquidExchangeConnector : public IExchangeConnector
{
 public:
  HyperliquidExchangeConnector(const HyperliquidConfig& config, BookUpdateBus* bookBus,
                               TradeBus* tradeBus, SymbolRegistry* symbolRegistry,
                               std::shared_ptr<ILogger> logger);

  // _pingThread is a joinable std::thread once start() has run; destroying it
  // joinable is std::terminate. stop() already joins it, so the destructor
  // only needs to make sure stop() has happened on every teardown path, not
  // just the ones that call it explicitly. Same pattern as
  // BybitExchangeConnector.
  ~HyperliquidExchangeConnector() override { stop(); }

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "hyperliquid"; }

  SymbolId resolveSymbolId(std::string_view symbol);

  // Public so the feed protocol handling (l2Book snapshots, trades) is
  // testable offline by feeding raw frames without a live socket. Same seam
  // BybitExchangeConnector exposes.
  void handleMessage(std::string_view payload);

  // Transport close. Called from the websocket onClose handler; public for
  // the same reason as handleMessage.
  void handleDisconnect(int code, std::string_view reason);

  void pollFeedHealth(MonoNanos now) override;

 private:
  HyperliquidConfig _config;

  BookUpdateBus* _bookBus;
  TradeBus* _tradeBus;
  SymbolRegistry* _registry{nullptr};

  // This connector's own id in the registry, resolved once in the
  // constructor. Every published event carries it as sourceExchange:
  // CompositeBookMatrix::onBookUpdate drops any update whose sourceExchange is
  // out of range, so leaving it at InvalidExchangeId kept the cross-venue book
  // permanently empty in live.
  ExchangeId _exchangeId{InvalidExchangeId};

  std::shared_ptr<ILogger> _logger;

  std::unique_ptr<IWebSocketClient> _wsClient;
  std::atomic<bool> _running{false};

  std::thread _pingThread;
  void pingLoop();

  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;
};

}  // namespace flox
