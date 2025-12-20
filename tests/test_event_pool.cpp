/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <flox/util/memory/pool.h>

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

}  // namespace

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
