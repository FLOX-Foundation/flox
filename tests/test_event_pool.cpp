/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <flox/util/eventing/event_bus.h>
#include <flox/util/memory/pool.h>

#include <atomic>
#include <vector>

using namespace flox;

namespace
{

class DummyEvent : public pool::PoolableBase<DummyEvent>
{
 public:
  explicit DummyEvent(std::pmr::memory_resource*) {}

  void clear() { cleared = true; }

  bool cleared = false;
};

// A pooled event carried through a bus. The bus destroys the handle sitting in
// a slot before it reuses the slot, which is what returns the object to its
// pool, so this is where the pool's release path and the bus's slot protocol
// meet.
struct BusEvent : public pool::PoolableBase<BusEvent>
{
  struct IBusListener
  {
    virtual ~IBusListener() = default;
    virtual void onBusEvent(const BusEvent& e) = 0;
  };
  using Listener = IBusListener;

  explicit BusEvent(std::pmr::memory_resource*) {}

  int value{0};
  uint64_t tickSequence{0};

  void clear() { value = 0; }
};

class CountingBusListener : public BusEvent::IBusListener
{
 public:
  void onBusEvent(const BusEvent&) override { ++count; }
  std::atomic<int> count{0};
};

}  // namespace

namespace flox
{
template <>
struct EventDispatcher<BusEvent>
{
  template <typename Sub>
  static void dispatch(const BusEvent& ev, Sub& sub)
  {
    sub.onBusEvent(ev);
  }
};
}  // namespace flox

TEST(EventPoolTest, AcquireReturnsValidHandle)
{
  pool::Pool<DummyEvent, 3> pool;

  auto h = pool.acquire();
  EXPECT_TRUE(h.has_value());
  EXPECT_NE(h.value().get(), nullptr);
}

TEST(EventPoolTest, ReleasingReturnsToPool)
{
  pool::Pool<DummyEvent, 1> pool;

  auto h1 = pool.acquire();
  EXPECT_TRUE(h1.has_value());

  DummyEvent* raw = h1.value().get();
  h1.reset();  // handle released

  auto h2 = pool.acquire();
  EXPECT_EQ(h2.value().get(), raw);  // reused
}

TEST(EventPoolTest, InUseIsTrackedCorrectly)
{
  pool::Pool<DummyEvent, 3> pool;

  EXPECT_EQ(pool.inUse(), 0u);

  auto h1 = pool.acquire();
  EXPECT_EQ(pool.inUse(), 1u);

  auto h2 = pool.acquire();
  EXPECT_EQ(pool.inUse(), 2u);

  h1.reset();
  EXPECT_EQ(pool.inUse(), 1u);

  h2.reset();
  EXPECT_EQ(pool.inUse(), 0u);
}

TEST(HandleTest, MoveReleasesPrevious)
{
  pool::Pool<DummyEvent, 1> pool;
  auto h1 = pool.acquire();
  DummyEvent* ptr = h1->get();
  EXPECT_EQ(ptr->refCount(), 1);

  {
    pool::Handle<DummyEvent> h2 = std::move(*h1);
    EXPECT_EQ(h1->get(), nullptr);
    EXPECT_EQ(h2.get(), ptr);
    EXPECT_EQ(ptr->refCount(), 1);
  }

  EXPECT_EQ(pool.inUse(), 0u);
}

TEST(HandleTest, DoubleMoveStillValid)
{
  pool::Pool<DummyEvent, 1> pool;

  {
    std::optional<pool::Handle<DummyEvent>> h1 = pool.acquire();

    EXPECT_TRUE(h1.has_value());
    DummyEvent* ptr = h1->get();

    pool::Handle<DummyEvent> h2 = std::move(*h1);
    pool::Handle<DummyEvent> h3 = std::move(h2);

    EXPECT_EQ(h3.get(), ptr);
    EXPECT_EQ(pool.inUse(), 1u);
  }

  EXPECT_EQ(pool.inUse(), 0u);
}

TEST(HandleTest, NullHandleIsSafe)
{
  std::optional<pool::Handle<DummyEvent>> h;
  EXPECT_FALSE(h.has_value());

  // Should not crash:
  h.reset();  // reassignment of nullptr
}

TEST(EventPoolTest, ClearIsCalledOnRelease)
{
  pool::Pool<DummyEvent, 1> pool;

  auto h = pool.acquire();
  DummyEvent* raw = h.value().get();
  EXPECT_FALSE(raw->cleared);

  h.reset();

  auto reused = pool.acquire();
  EXPECT_TRUE(reused.value().get()->cleared);
}

TEST(EventPoolTest, ExhaustionReturnsNullopt)
{
  pool::Pool<DummyEvent, 3> pool;

  auto h1 = pool.acquire();
  auto h2 = pool.acquire();
  auto h3 = pool.acquire();
  EXPECT_TRUE(h1.has_value());
  EXPECT_TRUE(h2.has_value());
  EXPECT_TRUE(h3.has_value());

  auto h4 = pool.acquire();
  EXPECT_FALSE(h4.has_value());
}

TEST(EventPoolTest, ExhaustionCallbackInvoked)
{
  pool::Pool<DummyEvent, 3> pool;

  // Test with static callback tracking (can't capture in C function pointer)
  static size_t s_capacity = 0;
  static size_t s_inUse = 0;
  static int s_count = 0;
  s_capacity = 0;
  s_inUse = 0;
  s_count = 0;

  pool.setExhaustionCallback([](size_t capacity, size_t inUse)
                             {
    s_capacity = capacity;
    s_inUse = inUse;
    ++s_count; });

  auto h1 = pool.acquire();
  auto h2 = pool.acquire();
  auto h3 = pool.acquire();
  EXPECT_EQ(s_count, 0);

  // Fourth acquire should trigger exhaustion callback
  auto h4 = pool.acquire();
  EXPECT_FALSE(h4.has_value());
  EXPECT_EQ(s_count, 1);
  EXPECT_EQ(s_capacity, 3u);
  EXPECT_EQ(s_inUse, 3u);

  // Fifth acquire should trigger again
  auto h5 = pool.acquire();
  EXPECT_EQ(s_count, 2);
}

TEST(EventPoolTest, ExhaustionCountTracked)
{
  pool::Pool<DummyEvent, 1> pool;

  EXPECT_EQ(pool.exhaustionCount(), 0u);

  auto h1 = pool.acquire();
  EXPECT_EQ(pool.exhaustionCount(), 0u);

  auto h2 = pool.acquire();  // exhausted
  EXPECT_FALSE(h2.has_value());
  EXPECT_EQ(pool.exhaustionCount(), 1u);

  auto h3 = pool.acquire();  // exhausted again
  EXPECT_EQ(pool.exhaustionCount(), 2u);

  h1.reset();  // release

  auto h4 = pool.acquire();  // should succeed now
  EXPECT_TRUE(h4.has_value());
  EXPECT_EQ(pool.exhaustionCount(), 2u);  // count unchanged
}

TEST(EventPoolTest, AcquireReleaseCountsTracked)
{
  pool::Pool<DummyEvent, 3> pool;

  EXPECT_EQ(pool.acquireCount(), 0u);
  EXPECT_EQ(pool.releaseCount(), 0u);

  auto h1 = pool.acquire();
  EXPECT_EQ(pool.acquireCount(), 1u);
  EXPECT_EQ(pool.releaseCount(), 0u);

  auto h2 = pool.acquire();
  EXPECT_EQ(pool.acquireCount(), 2u);

  h1.reset();
  EXPECT_EQ(pool.releaseCount(), 1u);

  h2.reset();
  EXPECT_EQ(pool.releaseCount(), 2u);
  EXPECT_EQ(pool.acquireCount(), 2u);
}

TEST(EventPoolTest, CapacityReturnsTemplateParam)
{
  pool::Pool<DummyEvent, 63> pool;
  EXPECT_EQ(pool.capacity(), 63u);
}

// Two pools of the same object type are ordinary: a Pool<BookUpdateEvent, N>
// is a connector member, and a process running two connectors holds two of
// them. Releasing into one of those pools has to reach that pool and no
// other.

TEST(EventPoolTest, ReleaseSurvivesAnotherPoolOfTheSameTypeBeingDestroyed)
{
  pool::Pool<DummyEvent, 64> keep;

  {
    pool::Pool<DummyEvent, 64> temp;
    auto th = temp.acquire();
    ASSERT_TRUE(th.has_value());
  }

  auto h = keep.acquire();
  ASSERT_TRUE(h.has_value());
  ASSERT_EQ(keep.inUse(), 1u);

  h.reset();

  EXPECT_EQ(keep.releaseCount(), 1u);
  EXPECT_EQ(keep.inUse(), 0u);
}

TEST(EventPoolTest, ReleaseGoesToTheOwningPoolNotTheLastConstructedOne)
{
  pool::Pool<DummyEvent, 64> big;
  pool::Pool<DummyEvent, 2> small;

  auto h = big.acquire();
  ASSERT_TRUE(h.has_value());
  h.reset();

  EXPECT_EQ(big.releaseCount(), 1u);
  EXPECT_EQ(big.inUse(), 0u);
  EXPECT_EQ(small.releaseCount(), 0u);
  EXPECT_EQ(small.acquireCount(), 0u);

  // The released slot has to come back. Writing the freelist at a foreign
  // pool's offsets loses it, and the count comes up one short.
  std::vector<pool::Handle<DummyEvent>> all;
  EXPECT_EQ(big.acquireBatch(all, 64), 64u);
}

// Every handle that goes through the bus has to come back to its pool, whether
// its slot was overwritten mid-run or drained when the bus stopped. A pool
// slot that never returns shows up here as a non-zero inUse() at the end.
TEST(EventPoolTest, HandlesPublishedThroughABusReturnToTheirPool)
{
  using Handle = pool::Handle<BusEvent>;

  pool::Pool<BusEvent, 64> objects;
  EventBus<Handle, 8> bus;
  CountingBusListener listener;

  bus.subscribe(&listener);
  bus.start();

  for (int i = 0; i < 500; ++i)
  {
    auto h = objects.acquire();
    ASSERT_TRUE(h.has_value());
    h->get()->value = i;
    bus.publish(std::move(*h));
  }

  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(), 500);
  EXPECT_EQ(objects.inUse(), 0u);
  EXPECT_EQ(objects.releaseCount(), objects.acquireCount());
}

// The same with a second pool of the same type alive alongside. Releases from
// the bus must land in the pool the object came from, not in whichever pool
// was constructed last.
TEST(EventPoolTest, BusReleasesReachTheOwningPoolWithASecondPoolAlive)
{
  using Handle = pool::Handle<BusEvent>;

  pool::Pool<BusEvent, 64> objects;
  pool::Pool<BusEvent, 4> bystander;

  EventBus<Handle, 8> bus;
  CountingBusListener listener;

  bus.subscribe(&listener);
  bus.start();

  for (int i = 0; i < 200; ++i)
  {
    auto h = objects.acquire();
    ASSERT_TRUE(h.has_value());
    h->get()->value = i;
    bus.publish(std::move(*h));
  }

  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(), 200);
  EXPECT_EQ(objects.inUse(), 0u);
  EXPECT_EQ(bystander.releaseCount(), 0u);
  EXPECT_EQ(bystander.inUse(), 0u);
}
