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

#include "flox/book/bus/book_update_bus.h"
#include "flox/book/events/book_update_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"

using namespace flox;
using namespace std::chrono_literals;

namespace
{

constexpr size_t PoolCapacity = 15;
using BookUpdatePool = pool::Pool<BookUpdateEvent, PoolCapacity>;

struct TickLogEntry
{
  int64_t tickId;
  SubscriberId subscriberId;
  TimePoint timestamp;
};

struct TimingSubscriber final : public IMarketDataSubscriber
{
  TimingSubscriber(SubscriberId id,
                   std::mutex& m,
                   std::vector<TickLogEntry>& log,
                   int sleepMs)
      : _id(id), _mutex(m), _log(log), _sleepMs(sleepMs) {}

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(_sleepMs));
    TickLogEntry e{ev.seq, _id, std::chrono::steady_clock::now()};
    std::lock_guard<std::mutex> lk(_mutex);
    _log.push_back(e);
  }

  SubscriberId id() const override { return _id; }

  SubscriberId _id;
  std::mutex& _mutex;
  std::vector<TickLogEntry>& _log;
  int _sleepMs;
};

}  // namespace

TEST(MarketDataBusTest, RequiredConsumersEnforceTickByTickOrdering)
{
  BookUpdateBus bus;
  BookUpdatePool pool;

  constexpr int numTicks = 5;

  std::mutex logMutex;
  std::vector<TickLogEntry> tickLog;

  auto fast = std::make_unique<TimingSubscriber>(1, logMutex, tickLog, 10);
  auto mid = std::make_unique<TimingSubscriber>(2, logMutex, tickLog, 30);
  auto slow = std::make_unique<TimingSubscriber>(3, logMutex, tickLog, 60);

  bus.subscribe(fast.get());
  bus.subscribe(mid.get());
  bus.subscribe(slow.get());

  bus.start();

  for (int i = 0; i < numTicks; ++i)
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    auto& ev = *h;

    ev->seq = i;
    ev->update.type = BookUpdateType::SNAPSHOT;
    ev->update.bids = {{Price::fromDouble(100.0 + i), Quantity::fromDouble(1.0)}};

    const auto seq = bus.publish(std::move(ev));
    bus.waitConsumed(seq);
  }

  bus.stop();

  std::map<uint64_t, std::vector<TimePoint>> tsByTick;
  for (const auto& e : tickLog)
  {
    tsByTick[e.tickId].push_back(e.timestamp);
  }

  for (uint64_t tick = 1; tick < static_cast<uint64_t>(numTicks); ++tick)
  {
    ASSERT_EQ(tsByTick[tick - 1].size(), 3u);
    ASSERT_EQ(tsByTick[tick].size(), 3u);

    const auto maxPrev = *std::max_element(tsByTick[tick - 1].begin(),
                                           tsByTick[tick - 1].end());
    const auto minCurr = *std::min_element(tsByTick[tick].begin(),
                                           tsByTick[tick].end());

    EXPECT_GE(minCurr, maxPrev) << "Tick " << tick
                                << " started before previous was fully processed";
  }

  EXPECT_EQ(pool.inUse(), 0u);
}
