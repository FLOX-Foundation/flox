/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/util/memory/ref_countable.h"

#include <gtest/gtest.h>

using namespace lrvx;

class TestRefCountable : public RefCountable
{
};

TEST(RefCountableTest, InitialCountIsZero)
{
  TestRefCountable obj;
  EXPECT_EQ(obj.refCount(), 0u);
}

TEST(RefCountableTest, RetainIncrementsRefCount)
{
  TestRefCountable obj;
  obj.resetRefCount(1);  // -> 1
  obj.retain();          // -> 2
  obj.retain();          // -> 3
  EXPECT_EQ(obj.refCount(), 3u);
}

TEST(RefCountableTest, ReleaseDecrementsRefCountAndReturnsFlag)
{
  TestRefCountable obj;
  obj.resetRefCount(2);
  EXPECT_FALSE(obj.release());  // -> 1
  EXPECT_TRUE(obj.release());   // -> 0
}

TEST(RefCountableTest, ResetSetsRefCount)
{
  TestRefCountable obj;
  obj.resetRefCount(7);
  EXPECT_EQ(obj.refCount(), 7u);
}

TEST(RefCountableDeathTest, ReleaseOnZeroRefCountTriggersAssert)
{
  TestRefCountable obj;
  ASSERT_DEATH(
      {
        obj.release();  // _refCount == 0 → assert
      },
      ".*");
}
