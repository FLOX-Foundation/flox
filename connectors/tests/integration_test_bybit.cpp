/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/bybit/bybit_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
#include <flox/book/events/trade_event.h>
#include <flox/common.h>
#include <flox/log/atomic_logger.h>
#include <flox/log/log.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace flox;

namespace
{

std::string getTempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

class CountingSub final : public IMarketDataSubscriber
{
 public:
  CountingSub(std::atomic<int64_t>& bookCount, std::atomic<int64_t>& tradeCount)
      : _book(bookCount), _trade(tradeCount)
  {
  }

  SubscriberId id() const override { return 99; }

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

TEST(BybitExchangeConnectorIntegrationTest, ReceivesDataFromBybit)
{
  std::atomic<int64_t> bookCounter{0};
  std::atomic<int64_t> tradeCounter{0};

  BookUpdateBus bookBus;
  TradeBus tradeBus;

  auto subscriber = std::make_unique<CountingSub>(bookCounter, tradeCounter);
  bookBus.subscribe(subscriber.get());
  tradeBus.subscribe(subscriber.get());
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;

  SymbolInfo btc{};
  btc.symbol = "BTCUSDT";
  btc.exchange = "bybit";
  btc.type = InstrumentType::Future;
  registry.registerSymbol(btc);

  SymbolInfo eth{};
  eth.symbol = "ETHUSDT";
  eth.exchange = "bybit";
  eth.type = InstrumentType::Future;
  registry.registerSymbol(eth);

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://stream.bybit.com/v5/public/linear";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top1},
                 {"ETHUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top1}};
  cfg.reconnectDelayMs = 2000;

  AtomicLoggerOptions logOpts;
  logOpts.directory = getTempLogDir();
  logOpts.basename = "bybit_test.log";
  logOpts.levelThreshold = LogLevel::Info;
  logOpts.maxFileSize = 5 * 1024 * 1024;
  logOpts.rotateInterval = std::chrono::minutes(10);
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  BybitExchangeConnector connector(cfg, &bookBus, &tradeBus, nullptr, &registry, logger);
  connector.start();

  std::this_thread::sleep_for(std::chrono::seconds(12));

  connector.stop();
  bookBus.stop();
  tradeBus.stop();

  FLOX_LOG("bookCounter.load(): " << bookCounter.load());
  FLOX_LOG("tradeCounter.load(): " << tradeCounter.load());

  EXPECT_GT(bookCounter.load(), 0) << "Expected to receive at least one book update";
  EXPECT_GT(tradeCounter.load(), 0) << "Expected to receive at least one trade";
}

TEST(BybitExchangeConnectorIntegrationTest, ReceivesSpotData)
{
  std::atomic<int64_t> bookCounter{0};
  std::atomic<int64_t> tradeCounter{0};

  BookUpdateBus bookBus;
  TradeBus tradeBus;

  auto subscriber = std::make_unique<CountingSub>(bookCounter, tradeCounter);
  bookBus.subscribe(subscriber.get());
  tradeBus.subscribe(subscriber.get());
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;

  SymbolInfo btc{};
  btc.symbol = "BTCUSDT";
  btc.exchange = "bybit";
  btc.type = InstrumentType::Spot;
  registry.registerSymbol(btc);

  SymbolInfo eth{};
  eth.symbol = "ETHUSDT";
  eth.exchange = "bybit";
  eth.type = InstrumentType::Spot;
  registry.registerSymbol(eth);

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://stream.bybit.com/v5/public/spot";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Spot, BybitConfig::BookDepth::Top200},
                 {"ETHUSDT", InstrumentType::Spot, BybitConfig::BookDepth::Top200}};
  cfg.reconnectDelayMs = 2000;

  AtomicLoggerOptions logOpts;
  logOpts.directory = getTempLogDir();
  logOpts.basename = "bybit_spot_test.log";
  logOpts.levelThreshold = LogLevel::Info;
  logOpts.maxFileSize = 5 * 1024 * 1024;
  logOpts.rotateInterval = std::chrono::minutes(10);
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  BybitExchangeConnector connector(cfg, &bookBus, &tradeBus, nullptr, &registry, logger);
  connector.start();

  std::this_thread::sleep_for(std::chrono::seconds(12));

  connector.stop();
  bookBus.stop();
  tradeBus.stop();

  FLOX_LOG("spot bookCounter.load(): " << bookCounter.load());
  FLOX_LOG("spot tradeCounter.load(): " << tradeCounter.load());

  EXPECT_GT(bookCounter.load(), 0) << "Expected to receive at least one spot book update";
  EXPECT_GT(tradeCounter.load(), 0) << "Expected to receive at least one spot trade";
}
