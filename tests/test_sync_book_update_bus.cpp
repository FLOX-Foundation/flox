/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
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

#ifndef USE_SYNC_BOOK_UPDATE_BUS
#error "Test requires USE_SYNC_BOOK_UPDATE_BUS to be defined"
#endif

namespace
{

constexpr size_t PoolCapacity = 15;
using BookUpdatePool = pool::Pool<BookUpdateEvent, PoolCapacity>;

struct TickLogEntry
{
  uint64_t tickId;
  SubscriberId subscriberId;
  TimePoint timestamp;
};

TEST(SyncMarketDataBusTest, DetectsAsyncBehaviorWithTimingGaps)
{
  BookUpdateBus bus;
  BookUpdatePool pool;

  constexpr int numTicks = 5;

  std::mutex logMutex;
  std::vector<TickLogEntry> tickLog;

  struct TimingSubscriber : public IMarketDataSubscriber
  {
    TimingSubscriber(SubscriberId id, std::mutex& mutex, std::vector<TickLogEntry>& log, int sleepMs)
        : _id(id), _mutex(mutex), _log(log), _sleepMs(sleepMs) {}

    void onBookUpdate(const BookUpdateEvent& ev) override
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(_sleepMs));
      TickLogEntry entry{ev.tickSequence, _id, std::chrono::steady_clock::now()};
      std::lock_guard<std::mutex> lock(_mutex);
      _log.push_back(entry);
    }

    SubscriberId id() const override { return _id; }

    SubscriberId _id;
    std::mutex& _mutex;
    std::vector<TickLogEntry>& _log;
    int _sleepMs;
  };

  auto fast = std::make_shared<TimingSubscriber>(1, logMutex, tickLog, 10);
  auto mid = std::make_shared<TimingSubscriber>(2, logMutex, tickLog, 30);
  auto slow = std::make_shared<TimingSubscriber>(3, logMutex, tickLog, 60);

  bus.subscribe(fast);
  bus.subscribe(mid);
  bus.subscribe(slow);

  bus.start();

  for (int i = 0; i < numTicks; ++i)
  {
    auto handleOpt = pool.acquire();
    ASSERT_TRUE(handleOpt.has_value());
    auto& handle = *handleOpt;

    handle->update.type = BookUpdateType::SNAPSHOT;
    handle->update.bids = {{Price::fromDouble(100.0 + i), Quantity::fromDouble(1.0)}};

    bus.publish(std::move(handle));
  }

  bus.stop();

  std::map<uint64_t, std::vector<TimePoint>> timestampsByTick;

  for (const auto& entry : tickLog)
  {
    timestampsByTick[entry.tickId].push_back(entry.timestamp);
  }

  for (uint64_t tick = 1; tick < numTicks; ++tick)
  {
    const auto& prev = timestampsByTick[tick - 1];
    const auto& curr = timestampsByTick[tick];

    ASSERT_EQ(prev.size(), 3);
    ASSERT_EQ(curr.size(), 3);

    auto maxPrev = *std::max_element(prev.begin(), prev.end());
    auto minCurr = *std::min_element(curr.begin(), curr.end());

    EXPECT_GE(minCurr, maxPrev) << "Tick " << tick << " started before previous was fully processed";
  }

  EXPECT_EQ(pool.inUse(), 0);
}

}  // namespace
