/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <cstring>

#include "flox/util/memory/large_arena.h"

using flox::memory::ArenaBacked;
using flox::memory::LargeArena;

TEST(LargeArena, AllocatesRoundsUpAndIsWritable)
{
  LargeArena arena(1);  // rounds up to one huge page
  ASSERT_NE(arena.data(), nullptr);
  EXPECT_EQ(arena.size(), LargeArena::kHugePageSize);

  // Whole region must be usable memory.
  std::memset(arena.data(), 0xAB, arena.size());
  auto* bytes = static_cast<unsigned char*>(arena.data());
  EXPECT_EQ(bytes[0], 0xAB);
  EXPECT_EQ(bytes[arena.size() - 1], 0xAB);
}

TEST(LargeArena, ReportsARealBackingMode)
{
  LargeArena arena(LargeArena::kHugePageSize);
  const std::string mode = arena.backingName();
  EXPECT_TRUE(mode == "hugetlb" || mode == "thp-advised" || mode == "plain");
#if defined(__APPLE__)
  EXPECT_EQ(mode, "plain");
#endif
}

TEST(LargeArena, AggregateCountersTrackLifetime)
{
  const auto before = LargeArena::totalBytesAll();
  {
    LargeArena arena(3 * LargeArena::kHugePageSize);
    EXPECT_EQ(LargeArena::totalBytesAll(), before + 3 * LargeArena::kHugePageSize);
    EXPECT_STRNE(LargeArena::aggregateMode(), "n/a");
  }
  EXPECT_EQ(LargeArena::totalBytesAll(), before);
}

namespace
{
struct BigInline
{
  static int liveCount;
  alignas(64) unsigned char storage[256 * 1024];
  int marker;

  explicit BigInline(int m) : marker(m)
  {
    ++liveCount;
    storage[0] = 1;
    storage[sizeof(storage) - 1] = 2;
  }
  ~BigInline() { --liveCount; }
};
int BigInline::liveCount = 0;
}  // namespace

TEST(LargeArena, ArenaBackedConstructsAndDestroysInPlace)
{
  {
    ArenaBacked<BigInline> obj(42);
    EXPECT_EQ(BigInline::liveCount, 1);
    EXPECT_EQ(obj->marker, 42);
    EXPECT_EQ(obj->storage[0], 1);
    EXPECT_EQ(obj->storage[sizeof(obj->storage) - 1], 2);
    // Object physically lives inside the arena region.
    const auto* base = static_cast<const unsigned char*>(
        const_cast<flox::memory::LargeArena&>(obj.arena()).data());
    const auto* objPtr = reinterpret_cast<const unsigned char*>(obj.get());
    EXPECT_GE(objPtr, base);
    EXPECT_LT(objPtr + sizeof(BigInline), base + obj.arena().size());
  }
  EXPECT_EQ(BigInline::liveCount, 0);
}
