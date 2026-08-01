/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <memory_resource>
#include <set>
#include <vector>

#include "flox/util/memory/pool.h"

namespace
{

using namespace flox;

struct PooledThing : public pool::PoolableBase<PooledThing>
{
  explicit PooledThing(std::pmr::memory_resource*) {}
  int payload{0};
};

// The only base a Handle can legally upcast to: RefCountable with a
// releaseToPool, i.e. the PoolableBase CRTP layer itself.
using PooledView = pool::PoolableBase<PooledThing>;

using ThingPool = pool::Pool<PooledThing, 15>;

TEST(PoolBatch, AcquireBatchYieldsRequestedCount)
{
  auto pool = std::make_unique<ThingPool>();
  std::vector<pool::Handle<PooledThing>> out;
  const size_t got = pool->acquireBatch(out, 10);
  EXPECT_EQ(got, 10u);
  ASSERT_EQ(out.size(), 10u);

  std::set<PooledThing*> distinct;
  for (auto& h : out)
  {
    ASSERT_NE(h.get(), nullptr);
    distinct.insert(h.get());
  }
  EXPECT_EQ(distinct.size(), 10u);
  EXPECT_EQ(pool->inUse(), 10u);
}

TEST(PoolBatch, ReleasingHandlesReturnsObjectsToPool)
{
  auto pool = std::make_unique<ThingPool>();
  {
    std::vector<pool::Handle<PooledThing>> out;
    ASSERT_EQ(pool->acquireBatch(out, 12), 12u);
    EXPECT_EQ(pool->inUse(), 12u);
  }
  EXPECT_EQ(pool->inUse(), 0u);

  // The whole capacity is reusable again.
  std::vector<pool::Handle<PooledThing>> out;
  EXPECT_EQ(pool->acquireBatch(out, 15), 15u);
}

TEST(PoolBatch, ExhaustionYieldsPartialBatch)
{
  auto pool = std::make_unique<ThingPool>();
  std::vector<pool::Handle<PooledThing>> held;
  ASSERT_EQ(pool->acquireBatch(held, 10), 10u);

  std::vector<pool::Handle<PooledThing>> more;
  const size_t got = pool->acquireBatch(more, 10);
  EXPECT_EQ(got, 5u);  // only 5 left
  EXPECT_EQ(pool->inUse(), 15u);
  EXPECT_EQ(pool->exhaustionCount(), 1u);
}

TEST(PoolBatch, InteroperatesWithSingleAcquire)
{
  auto pool = std::make_unique<ThingPool>();
  auto single = pool->acquire();
  ASSERT_TRUE(single.has_value());

  std::vector<pool::Handle<PooledThing>> out;
  EXPECT_EQ(pool->acquireBatch(out, 5), 5u);
  EXPECT_EQ(pool->inUse(), 6u);

  for (auto& h : out)
  {
    EXPECT_NE(h.get(), single->get());
  }
}

TEST(PoolBatch, UpcastRetainsExactlyOnce)
{
  auto pool = std::make_unique<ThingPool>();
  {
    auto handle = pool->acquire();
    ASSERT_TRUE(handle.has_value());
    {
      auto base = handle->upcast<PooledView>();
      EXPECT_EQ(base.get(), static_cast<PooledView*>(handle->get()));
      EXPECT_EQ(pool->inUse(), 1u);
    }
    // Dropping the upcast handle must not release the object early.
    EXPECT_EQ(pool->inUse(), 1u);
  }
  // Dropping the last handle returns the object; a double retain in
  // upcast() would leak it here and inUse() would stay at 1.
  EXPECT_EQ(pool->inUse(), 0u);
}

}  // namespace
