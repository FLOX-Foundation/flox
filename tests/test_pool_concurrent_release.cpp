/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A pooled event is returned to its pool by whichever thread overwrites the bus
// slot holding it -- not by the thread that acquired it. With several producers
// sharing a bus, that means release() runs concurrently from foreign threads,
// which the pool's original SPSC freelist did not allow (ThreadSanitizer caught
// it as a data race once the demo was fixed and started generating contention).
//
// This test reproduces that topology directly: two publisher threads, one bus,
// one shared pool.

#include "flox/book/events/book_update_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/util/eventing/event_bus.h"
#include "flox/util/memory/pool.h"

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace
{

using namespace flox;

constexpr size_t kRingCapacity = 64;
constexpr size_t kPoolCapacity = 255;
constexpr int kPublishersCount = 2;
constexpr int kEventsPerPublisher = 20000;

using BookPool = pool::Pool<BookUpdateEvent, kPoolCapacity>;
using TestBus = EventBus<pool::Handle<BookUpdateEvent>, kRingCapacity>;

struct CountingSink : public IMarketDataSubscriber
{
  SubscriberId id() const override { return 1; }
  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    delivered.fetch_add(1, std::memory_order_relaxed);
    levels.fetch_add(ev.update.bids.size(), std::memory_order_relaxed);
  }
  std::atomic<long> delivered{0};
  std::atomic<long> levels{0};
};

}  // namespace

TEST(PoolConcurrentRelease, TwoPublishersShareOneBusAndPool)
{
  auto pool = std::make_unique<BookPool>();
  TestBus bus;
  CountingSink sink;
  bus.subscribe(&sink);
  bus.start();

  std::atomic<long> published{0};

  std::vector<std::thread> publishers;
  for (int p = 0; p < kPublishersCount; ++p)
  {
    publishers.emplace_back(
        [&, p]
        {
          for (int i = 0; i < kEventsPerPublisher; ++i)
          {
            auto handle = pool->acquire();
            if (!handle)
            {
              continue;
            }
            auto& ev = **handle;
            ev.update.symbol = static_cast<SymbolId>(p);
            ev.update.type = BookUpdateType::DELTA;
            // Vary the depth so the pooled vectors keep reallocating. Both
            // publishers then hit the pool's shared pmr resource concurrently,
            // which is the second half of this bug: an unsynchronized resource
            // can hand the same block to two threads.
            const int depth = 1 + (i % 48);
            for (int level = 0; level < depth; ++level)
            {
              ev.update.bids.emplace_back(Price::fromDouble(100.0 - level),
                                          Quantity::fromDouble(1.0));
            }
            bus.publish(std::move(*handle));
            published.fetch_add(1, std::memory_order_relaxed);
          }
        });
  }
  for (auto& t : publishers)
  {
    t.join();
  }
  bus.stop();

  // Objects must have gone back and forth rather than the pool draining, or
  // the run proves nothing about concurrent release.
  EXPECT_EQ(pool->exhaustionCount(), 0u) << "pool drained -- publishers were starved, not raced";
  EXPECT_GT(pool->releaseCount(), static_cast<size_t>(kPublishersCount * kEventsPerPublisher / 2))
      << "objects did not circulate back to the pool";

  // Every acquired object is accounted for: none lost, none double-freed.
  EXPECT_GE(pool->acquireCount(), pool->releaseCount());
  EXPECT_LE(pool->acquireCount() - pool->releaseCount(), kRingCapacity)
      << "more objects outstanding than the ring can hold -- freelist lost or duplicated slots";

  EXPECT_EQ(published.load(), static_cast<long>(kPublishersCount * kEventsPerPublisher));
  EXPECT_GT(sink.delivered.load(), 0);
}
