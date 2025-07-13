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
