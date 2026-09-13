/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/aggregator/bar_matrix.h"
#include "flox/aggregator/timeframe.h"
#include "flox/common.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>

using namespace flox;

TEST(BarMatrixSizing, StorageGrowsWithTheProductOfItsParameters)
{
  using Small = BarMatrix<16, 2, 8>;
  EXPECT_EQ(Small::kStorageBytes, std::size_t{16} * 2 * 8 * sizeof(Bar));
  EXPECT_GE(sizeof(Small), Small::kStorageBytes);

  using Wider = BarMatrix<32, 2, 8>;
  EXPECT_EQ(Wider::kStorageBytes, 2 * Small::kStorageBytes);
}

// The default parameters describe 256 symbols x 8 timeframes x 256 bars, which
// is tens of megabytes. Nothing in C++ can stop a caller from declaring one as
// a local, so the size is published as a constant and spelled out in the header
// comment instead.
TEST(BarMatrixSizing, DefaultParametersDoNotFitAnyThreadStack)
{
  constexpr std::size_t kMacOsMainStack = 8ull * 1024 * 1024;
  EXPECT_GT(BarMatrix<>::kStorageBytes, kMacOsMainStack);

  constexpr std::size_t kTypicalWorkerStack = 512ull * 1024;
  EXPECT_GT((BarMatrix<256, 4, 64>::kStorageBytes), kTypicalWorkerStack);
}

TEST(BarMatrixSizing, HeapAllocatedMatrixStoresAndReadsBars)
{
  auto matrix = std::make_unique<BarMatrix<16, 2, 8>>();
  const std::array<TimeframeId, 1> tfs{TimeframeId::time(std::chrono::seconds{60})};
  matrix->configure(tfs);

  BarEvent ev{};
  ev.symbol = 1;
  ev.barType = tfs[0].type;
  ev.barTypeParam = tfs[0].param;
  ev.bar.close = Price::fromDouble(100.0);
  matrix->onBar(ev);

  const Bar* stored = matrix->bar(1, tfs[0]);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->close, Price::fromDouble(100.0));
}
