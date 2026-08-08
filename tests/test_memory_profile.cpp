/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include "flox/util/performance/memory_profile.h"

using flox::performance::applyMemoryProfile;
using flox::performance::MemoryProfile;
using flox::performance::memoryProfileFromString;

TEST(MemoryProfile, FromStringParsesKnownValuesAndDefaultsSafely)
{
  EXPECT_EQ(memoryProfileFromString("colo"), MemoryProfile::COLO);
  EXPECT_EQ(memoryProfileFromString("default"), MemoryProfile::DEFAULT);
  EXPECT_EQ(memoryProfileFromString(""), MemoryProfile::DEFAULT);
  EXPECT_EQ(memoryProfileFromString("garbage"), MemoryProfile::DEFAULT);
}

TEST(MemoryProfile, DefaultProfileRequestsNothing)
{
  const auto r = applyMemoryProfile(MemoryProfile::DEFAULT);
  EXPECT_EQ(r.profile, MemoryProfile::DEFAULT);
  EXPECT_FALSE(r.mlockRequested);
  EXPECT_FALSE(r.mlockApplied);
  EXPECT_TRUE(r.mlockError.empty());
  EXPECT_NE(r.toString().find("mlock=not-requested"), std::string::npos);
}

TEST(MemoryProfile, ColoProfileIsRequestedAndReportIsConsistent)
{
  // Whether mlockall succeeds depends on host privileges; the contract under
  // test is consistency, not success: a failure must carry an errno text and
  // must never be silent.
  const auto r = applyMemoryProfile(MemoryProfile::COLO);
  EXPECT_EQ(r.profile, MemoryProfile::COLO);
  EXPECT_TRUE(r.mlockRequested);
  if (r.mlockApplied)
  {
    EXPECT_TRUE(r.mlockError.empty());
    EXPECT_NE(r.toString().find("mlock=applied"), std::string::npos);
  }
  else
  {
    EXPECT_FALSE(r.mlockError.empty());
    EXPECT_NE(r.toString().find("mlock=FAILED"), std::string::npos);
  }

#if defined(__linux__) || defined(__APPLE__)
  EXPECT_TRUE(r.memlockLimitKnown);
#endif

  // Do not leave the test process locked.
#if defined(__linux__) || defined(__APPLE__)
  ::munlockall();
#endif
}

TEST(MemoryProfile, ReportMentionsEveryField)
{
  const auto r = applyMemoryProfile(MemoryProfile::DEFAULT);
  const auto s = r.toString();
  EXPECT_NE(s.find("memory profile="), std::string::npos);
  EXPECT_NE(s.find("memlock-limit="), std::string::npos);
  EXPECT_NE(s.find("huge-arena="), std::string::npos);
}
