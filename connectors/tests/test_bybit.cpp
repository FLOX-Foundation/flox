/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/bybit/bybit_exchange_connector.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/log/abstract_logger.h"
#include "flox/log/atomic_logger.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>

using namespace flox;

class CountingSubscriber : public IMarketDataSubscriber
{
 public:
  CountingSubscriber(SubscriberId id, std::atomic<int64_t>& bookCounter,
                     std::atomic<int64_t>& tradeCounter)
      : _id(id), _bookCounter(bookCounter), _tradeCounter(tradeCounter) {}

  SubscriberId id() const override { return _id; }
  SubscriberMode mode() const override { return SubscriberMode::PUSH; }

  void onBookUpdate(const BookUpdateEvent&) override
  {
    _bookCounter.fetch_add(1, std::memory_order_relaxed);
  }

  void onTrade(const TradeEvent&) override
  {
    _tradeCounter.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  SubscriberId _id;
  std::atomic<int64_t>& _bookCounter;
  std::atomic<int64_t>& _tradeCounter;
};

TEST(BybitConnectorTest, ReceivesMarketData)
{
  std::atomic<int64_t> bookCount{0};
  std::atomic<int64_t> tradeCount{0};

  BookUpdateBus bookUpdateBus;
  TradeBus tradeBus;

  auto subscriber =
      std::make_shared<CountingSubscriber>(42, bookCount, tradeCount);
  bookUpdateBus.subscribe(subscriber);
  tradeBus.subscribe(subscriber);
  bookUpdateBus.start();
  tradeBus.start();

  BybitConfig config = {
      .endpoint = "wss://stream.bybit.com/v5/public/linear",
      .symbols = {"BTCUSDT", "ETHUSDT"},
      .reconnectDelayMs = 2000};

  std::unordered_map<std::string, SymbolId> symbolMap = {
      {"BTCUSDT", 1},
      {"ETHUSDT", 2},
  };

  auto getSymbolId = [&](std::string_view s)
  {
    auto it = symbolMap.find(std::string(s));
    return (it != symbolMap.end()) ? it->second : SymbolId{0};
  };

  AtomicLoggerOptions logOpts;
  logOpts.basename = "bybit.log";
  logOpts.directory = "/dev/shm/logs";
  logOpts.levelThreshold = LogLevel::Info;
  logOpts.maxFileSize = 10 * 1024 * 1024;
  logOpts.rotateInterval = std::chrono::minutes(15);

  auto logger = std::make_shared<AtomicLogger>(logOpts);

  struct SLogger : public ILogger
  {
    void info(std::string_view msg)
    {
      std::cout << msg << std::endl;
    }
    void warn(std::string_view msg)
    {
      std::cout << msg << std::endl;
    }
    void error(std::string_view msg)
    {
      std::cout << msg << std::endl;
    }
  };

  BybitExchangeConnector connector(
      config,
      &bookUpdateBus,
      &tradeBus,
      getSymbolId,
      std::make_shared<SLogger>());

  connector.start();

  std::this_thread::sleep_for(std::chrono::seconds(10));

  connector.stop();
  bookUpdateBus.stop();
  tradeBus.stop();

  EXPECT_GT(bookCount.load(), 1) << "Expected some book updates";
  EXPECT_GT(tradeCount.load(), 1) << "Expected some trade events";
}
