/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Pool object lifetime and the acquire/release accounting.
//
// Two things the pool does not do today. It placement-news every slot in its
// constructor and destroys none of them: ~Pool() is defaulted over a raw
// array of Storage, so a pooled type holding anything that is not the pmr
// arena -- a std::string, a shared_ptr, a file descriptor -- leaks one per
// slot, per pool, for the life of the process. And release() is unguarded:
// it pushes the slot back on the freelist and bumps a counter whatever state
// the object was in, so a second release of the same object puts one index on
// the freelist twice (two acquirers get the same object) and drives inUse()'s
// `acquired - released` subtraction below zero, where size_t turns it into a
// number near 2^64 that is then handed to the exhaustion callback.

#include <gtest/gtest.h>

#include <flox/util/memory/pool.h>

#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <set>
#include <string>
#include <vector>

using namespace flox;

namespace
{

// Counts its own constructions and destructions, and holds a heap resource
// that only a destructor can give back.
struct LifetimeCounters
{
  static inline std::atomic<int> constructed{0};
  static inline std::atomic<int> destructed{0};

  static void reset()
  {
    constructed.store(0);
    destructed.store(0);
  }
};

class CountedEvent : public pool::PoolableBase<CountedEvent>
{
 public:
  explicit CountedEvent(std::pmr::memory_resource*)
      : payload("a resource the arena does not own")
  {
    LifetimeCounters::constructed.fetch_add(1);
  }

  ~CountedEvent() { LifetimeCounters::destructed.fetch_add(1); }

  void clear() { payload.assign("a resource the arena does not own"); }

  std::string payload;
};

constexpr size_t kCapacity = 8;
using CountedPool = pool::Pool<CountedEvent, kCapacity>;

}  // namespace

// The green control: the acquire/release bookkeeping of an ordinary,
// well-behaved cycle is already right, so the failures below are about the
// two specific holes and not about the pool in general.
TEST(PoolObjectLifetime, OrdinaryAcquireAndReleaseAccountsForEverySlot)
{
  LifetimeCounters::reset();
  CountedPool pool;

  EXPECT_EQ(pool.capacity(), kCapacity);
  EXPECT_EQ(pool.inUse(), 0u);

  {
    std::vector<pool::Handle<CountedEvent>> held;
    for (size_t i = 0; i < kCapacity; ++i)
    {
      auto h = pool.acquire();
      ASSERT_TRUE(h.has_value()) << "slot " << i << " should still be free";
      held.push_back(std::move(*h));
    }
    EXPECT_EQ(pool.inUse(), kCapacity);
    EXPECT_FALSE(pool.acquire().has_value()) << "an exhausted pool hands out nothing";
    EXPECT_EQ(pool.exhaustionCount(), 1u);
  }

  EXPECT_EQ(pool.inUse(), 0u);
  EXPECT_EQ(pool.acquireCount(), kCapacity);
  EXPECT_EQ(pool.releaseCount(), kCapacity);
}

TEST(PoolObjectLifetime, EverySlotIsDestroyedExactlyOnceWhenThePoolDies)
{
  LifetimeCounters::reset();

  {
    CountedPool pool;
    ASSERT_EQ(LifetimeCounters::constructed.load(), static_cast<int>(kCapacity))
        << "the pool constructs one object per slot up front";

    // A full round trip, so the slots are in the ordinary post-release state
    // rather than untouched since construction.
    for (size_t i = 0; i < kCapacity; ++i)
    {
      auto h = pool.acquire();
      ASSERT_TRUE(h.has_value());
    }
    EXPECT_EQ(LifetimeCounters::destructed.load(), 0)
        << "returning an object to the pool must not destroy it: the slot is "
           "reused in place";
  }

  EXPECT_EQ(LifetimeCounters::destructed.load(), static_cast<int>(kCapacity))
      << "~Pool() destroyed " << LifetimeCounters::destructed.load() << " of "
      << kCapacity
      << " objects: the slots are raw storage the pool placement-newed into and "
         "never unwinds, so everything a pooled object owns outside the arena "
         "is leaked once per slot";
  EXPECT_EQ(LifetimeCounters::destructed.load(), LifetimeCounters::constructed.load())
      << "constructions and destructions must balance exactly once each";
}

TEST(PoolObjectLifetime, ADoubleReleaseIsRefusedAndInUseNeverGoesNegative)
{
  LifetimeCounters::reset();
  CountedPool pool;

  CountedEvent* raw = nullptr;
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    raw = h->get();
  }  // the handle drops the last reference: one legitimate release

  ASSERT_EQ(pool.acquireCount(), 1u);
  ASSERT_EQ(pool.releaseCount(), 1u);
  ASSERT_EQ(pool.inUse(), 0u);

  // The same object handed back a second time -- a stale Handle copy, a
  // bus slot destroyed twice, a connector that releases what it already
  // published. The pool must refuse it; today it takes it.
  pool.release(raw);

  EXPECT_EQ(pool.releaseCount(), 1u)
      << "the pool accepted a release of an object that was not in use, so the "
         "same slot index now sits on the freelist twice and two acquirers "
         "will be handed the same object";
  EXPECT_EQ(pool.inUse(), 0u)
      << "inUse() reported " << pool.inUse()
      << ": `acquired - released` underflowed in size_t, and that value is what "
         "the exhaustion callback is given";
  EXPECT_LE(pool.inUse(), pool.capacity())
      << "inUse() can never exceed the pool's own capacity";
}

TEST(PoolObjectLifetime, TheFreelistNeverHandsOutOneSlotTwice)
{
  LifetimeCounters::reset();
  CountedPool pool;

  CountedEvent* raw = nullptr;
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    raw = h->get();
  }
  pool.release(raw);  // the bogus second release

  // Whatever the pool did with that release, it must not now believe it owns
  // more free slots than it has.
  std::vector<pool::Handle<CountedEvent>> held;
  std::vector<CountedEvent*> seen;
  for (size_t i = 0; i < kCapacity + 2; ++i)
  {
    auto h = pool.acquire();
    if (!h.has_value())
    {
      break;
    }
    seen.push_back(h->get());
    held.push_back(std::move(*h));
  }

  EXPECT_LE(seen.size(), kCapacity) << "the pool handed out " << seen.size()
                                    << " objects out of " << kCapacity << " slots";

  std::set<CountedEvent*> distinct(seen.begin(), seen.end());
  EXPECT_EQ(distinct.size(), seen.size())
      << seen.size() << " acquisitions returned only " << distinct.size()
      << " distinct objects: the same slot index sits on the freelist more than "
         "once, so two live acquirers are writing the same event";
}
