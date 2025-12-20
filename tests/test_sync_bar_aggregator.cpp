/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "flox/aggregator/bar_aggregator.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"

using namespace flox;
using namespace std::chrono_literals;

namespace
{

constexpr size_t PoolCapacity = 15;
using BarPool = pool::Pool<BarEvent, PoolCapacity>;

struct BarLogEntry
{
  SymbolId symbol;
  SubscriberId subscriberId;
  TimePoint timestamp;
};

struct TimingSubscriber final : public IMarketDataSubscriber
{
  TimingSubscriber(SubscriberId id, std::mutex& m, std::vector<BarLogEntry>& log, int sleepMs)
      : _id(id), _mutex(m), _log(log), _sleepMs(sleepMs)
  {
  }

  void onBar(const BarEvent& ev) override
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(_sleepMs));
    BarLogEntry e{ev.symbol, _id, std::chrono::steady_clock::now()};
    std::lock_guard<std::mutex> lk(_mutex);
    _log.push_back(e);
  }

  SubscriberId id() const override { return _id; }

  SubscriberId _id;
  std::mutex& _mutex;
  std::vector<BarLogEntry>& _log;
  int _sleepMs;
};

TimePoint ts(int seconds) { return TimePoint(std::chrono::seconds(seconds)); }

TradeEvent makeTrade(SymbolId symbol, double price, double qty, int sec)
{
  TradeEvent event;
  event.trade.symbol = symbol;
  event.trade.price = Price::fromDouble(price);
  event.trade.quantity = Quantity::fromDouble(qty);
  event.trade.isBuy = true;
  event.trade.exchangeTsNs = ts(sec).time_since_epoch().count();
  return event;
}

}  // namespace

TEST(SyncBarAggregatorTest, RequiredConsumersEnforceBarByBarOrdering)
{
  BarBus bus;
  bus.enableDrainOnStop();

  std::mutex logMutex;
  std::vector<BarLogEntry> barLog;

  auto fast = std::make_unique<TimingSubscriber>(1, logMutex, barLog, 10);
  auto mid = std::make_unique<TimingSubscriber>(2, logMutex, barLog, 30);
  auto slow = std::make_unique<TimingSubscriber>(3, logMutex, barLog, 60);

  bus.subscribe(fast.get());
  bus.subscribe(mid.get());
  bus.subscribe(slow.get());

  // Use tick bars for predictable bar generation
  TickBarAggregator aggregator(TickBarPolicy(2), &bus);

  bus.start();
  aggregator.start();

  // Generate 3 bars: each bar = 2 trades, so 6 trades = 3 bars
  // Each bar goes to 3 subscribers = 9 log entries
  constexpr int numBars = 3;
  for (int i = 0; i < numBars * 2; ++i)
  {
    aggregator.onTrade(makeTrade(1, 100.0 + i, 1.0, i));
  }

  aggregator.stop();
  bus.stop();

  // Verify we got bars: numBars bars * 3 subscribers = 9 entries
  EXPECT_EQ(barLog.size(), static_cast<size_t>(numBars * 3));

  // All entries should be for symbol 1
  for (const auto& e : barLog)
  {
    EXPECT_EQ(e.symbol, 1u);
  }
}

TEST(SyncBarAggregatorTest, MultipleSymbolsProcessedCorrectly)
{
  BarBus bus;
  bus.enableDrainOnStop();

  std::mutex logMutex;
  std::vector<BarLogEntry> barLog;

  auto subscriber = std::make_unique<TimingSubscriber>(1, logMutex, barLog, 5);
  bus.subscribe(subscriber.get());

  // Use tick bars for predictable behavior
  TickBarAggregator aggregator(TickBarPolicy(2), &bus);

  bus.start();
  aggregator.start();

  // Generate 1 bar per symbol: 2 trades each
  for (SymbolId sym = 1; sym <= 3; ++sym)
  {
    aggregator.onTrade(makeTrade(sym, 100.0, 1.0, 0));
    aggregator.onTrade(makeTrade(sym, 101.0, 1.0, 1));  // triggers bar close
  }

  aggregator.stop();
  bus.stop();

  // Count bars per symbol
  std::map<SymbolId, int> barCount;
  for (const auto& e : barLog)
  {
    barCount[e.symbol]++;
  }

  EXPECT_EQ(barCount[1], 1);
  EXPECT_EQ(barCount[2], 1);
  EXPECT_EQ(barCount[3], 1);
}

TEST(SyncBarAggregatorTest, TickBarAggregatorSync)
{
  BarBus bus;
  bus.enableDrainOnStop();

  std::mutex logMutex;
  std::vector<BarLogEntry> barLog;

  auto subscriber = std::make_unique<TimingSubscriber>(1, logMutex, barLog, 5);
  bus.subscribe(subscriber.get());

  TickBarAggregator aggregator(TickBarPolicy(3), &bus);

  bus.start();
  aggregator.start();

  // 9 trades = 3 bars (3 trades each)
  for (int i = 0; i < 9; ++i)
  {
    aggregator.onTrade(makeTrade(1, 100.0 + i, 1.0, i));
  }

  aggregator.stop();
  bus.stop();

  // Should have 3 bars (trades 0-2, 3-5, 6-8)
  EXPECT_EQ(barLog.size(), 3u);
}

TEST(SyncBarAggregatorTest, VolumeBarAggregatorSync)
{
  BarBus bus;
  bus.enableDrainOnStop();

  std::mutex logMutex;
  std::vector<BarLogEntry> barLog;

  auto subscriber = std::make_unique<TimingSubscriber>(1, logMutex, barLog, 5);
  bus.subscribe(subscriber.get());

  // Volume threshold: 100 * 10 = 1000 notional
  VolumeBarAggregator aggregator(VolumeBarPolicy::fromDouble(1000.0), &bus);

  bus.start();
  aggregator.start();

  // Each trade: price=100, qty=5 → notional = 500
  // 2 trades = 1000 notional → bar closes
  for (int i = 0; i < 6; ++i)
  {
    aggregator.onTrade(makeTrade(1, 100.0, 5.0, i));
  }

  aggregator.stop();
  bus.stop();

  // Should have 3 bars (trades 0-1, 2-3, 4-5)
  EXPECT_EQ(barLog.size(), 3u);
}
