/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "flox/common.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
#include <flox/book/events/trade_event.h>
#include <flox/engine/symbol_registry.h>
#include <flox/log/atomic_logger.h>
#include <flox/log/log.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace flox;

namespace
{

class CountingSub final : public IMarketDataSubscriber
{
 public:
  CountingSub(std::atomic<int64_t>& bookCount, std::atomic<int64_t>& tradeCount)
      : _book(bookCount), _trade(tradeCount)
  {
  }

  SubscriberId id() const override { return 99; }
  SubscriberMode mode() const override { return SubscriberMode::PUSH; }

  void onBookUpdate(const BookUpdateEvent&) override
  {
    _book.fetch_add(1, std::memory_order_relaxed);
  }

  void onTrade(const TradeEvent&) override { _trade.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::atomic<int64_t>& _book;
  std::atomic<int64_t>& _trade;
};

}  // namespace

TEST(HyperliquidExchangeConnectorIntegrationTest, ReceivesDataFromHyperliquid)
{
  SymbolRegistry registry;

  SymbolInfo btcInfo;
  btcInfo.symbol = "BTC";
  btcInfo.exchange = "hyperliquid";
  btcInfo.type = InstrumentType::Future;

  SymbolInfo ethInfo;
  ethInfo.symbol = "ETH";
  ethInfo.exchange = "hyperliquid";
  ethInfo.type = InstrumentType::Future;

  SymbolId btcId = registry.registerSymbol(btcInfo);
  SymbolId ethId = registry.registerSymbol(ethInfo);

  std::atomic<int64_t> bookCounter{0};
  std::atomic<int64_t> tradeCounter{0};

  BookUpdateBus bookBus;
  TradeBus tradeBus;

  auto subscriber = std::make_shared<CountingSub>(bookCounter, tradeCounter);
  bookBus.subscribe(subscriber);
  tradeBus.subscribe(subscriber);
  bookBus.start();
  tradeBus.start();

  HyperliquidConfig cfg;
  cfg.wsEndpoint = "wss://api.hyperliquid.xyz/ws";
  cfg.restEndpoint = "https://api.hyperliquid.xyz";
  cfg.symbols = {"BTC", "ETH"};
  cfg.reconnectDelayMs = 2000;

  AtomicLoggerOptions logOpts;
  logOpts.directory = "/dev/shm/logs";
  logOpts.basename = "hyperliquid_test.log";
  logOpts.levelThreshold = LogLevel::Info;
  logOpts.maxFileSize = 5 * 1024 * 1024;
  logOpts.rotateInterval = std::chrono::minutes(10);
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  HyperliquidExchangeConnector connector(cfg, &bookBus, &tradeBus, &registry, logger);
  connector.start();

  std::this_thread::sleep_for(std::chrono::seconds(12));

  connector.stop();
  bookBus.stop();
  tradeBus.stop();

  EXPECT_GT(bookCounter.load(), 0) << "Expected to receive at least one book update";
  EXPECT_GT(tradeCounter.load(), 0) << "Expected to receive at least one trade";
}
