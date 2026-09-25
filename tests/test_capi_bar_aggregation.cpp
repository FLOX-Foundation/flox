/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// flox_aggregate_renko_bars() is the one bar-aggregation C function that can
// close more than one bar per input trade: a trade that gaps past several
// brick widths closes the brick that was forming and also synthesizes the
// bricks in between. Every binding that calls it (Node, QuickJS) used to
// size its output buffer to the trade count, matching what every other
// aggregate_* function needs -- a safe assumption for them, since they never
// emit more than one bar per trade, but not for Renko once gap synthesis
// landed. This pins the contract the bindings now rely on: the function
// reports the true bar count even when it exceeds `max_bars`, and writes
// only as many bars as fit, so a caller comparing the return value against
// its buffer size knows to retry with more room instead of reading (or, as
// in Node's case, walking off the end of) a buffer that was too small.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{

// A trade sequence with a single gap from 100 to 155 at a brick size of 10:
// 5.5 brick-widths, so 5 complete bricks close (the real one plus 4
// synthesized) and a new bar is left open at 155.
struct GapTape
{
  std::vector<int64_t> ts{0, 1'000'000'000};
  std::vector<double> px{100.0, 155.0};
  std::vector<double> qty{1.0, 1.0};
  std::vector<uint8_t> isBuy{1, 1};
};

}  // namespace

// Sizing bars_out to the trade count -- the assumption every other
// aggregate_* function makes safe -- undercounts Renko once a gap spans
// several bricks. The call must not overrun bars_out (bounds-checked
// inside doAggregateC), but it must still report the true total so the
// caller can tell the result was truncated.
TEST(CapiBarAggregationTest, RenkoReportsTrueCountEvenWhenBufferIsTooSmall)
{
  GapTape tape;
  const auto n = static_cast<uint32_t>(tape.ts.size());

  std::vector<FloxBar> bars(n);  // sized the way every non-Renko caller does
  const uint32_t count = flox_aggregate_renko_bars(tape.ts.data(), tape.px.data(), tape.qty.data(),
                                                   tape.isBuy.data(), tape.ts.size(), 10.0, bars.data(), n);

  EXPECT_GT(count, n) << "the 5.5-brick gap should report more bars than there were trades (2)";
  EXPECT_EQ(count, 5u) << "1 real brick + 4 synthesized bricks";
}

// Retrying with a buffer sized to the reported count -- the fix applied to
// the Node and QuickJS bindings -- recovers every bar, in the right order
// and with the right prices.
TEST(CapiBarAggregationTest, RenkoRetryWithReportedCountRecoversAllBars)
{
  GapTape tape;
  const auto n = static_cast<uint32_t>(tape.ts.size());

  std::vector<FloxBar> firstPass(n);
  uint32_t count = flox_aggregate_renko_bars(tape.ts.data(), tape.px.data(), tape.qty.data(), tape.isBuy.data(),
                                             tape.ts.size(), 10.0, firstPass.data(), n);
  ASSERT_GT(count, n);

  std::vector<FloxBar> bars(count);
  const uint32_t recount = flox_aggregate_renko_bars(tape.ts.data(), tape.px.data(), tape.qty.data(),
                                                     tape.isBuy.data(), tape.ts.size(), 10.0, bars.data(), count);

  ASSERT_EQ(recount, count);
  ASSERT_EQ(bars.size(), 5u);

  static constexpr double kScale = 1e8;
  // The brick that was forming closes at the boundary the gapping trade
  // crossed (110), not at the price it held before that trade was applied.
  EXPECT_DOUBLE_EQ(bars[0].open_raw / kScale, 100.0);
  EXPECT_DOUBLE_EQ(bars[0].close_raw / kScale, 110.0);

  const double expectedOpens[] = {110.0, 120.0, 130.0, 140.0};
  const double expectedCloses[] = {120.0, 130.0, 140.0, 150.0};
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_DOUBLE_EQ(bars[i + 1].open_raw / kScale, expectedOpens[i]) << "brick " << i;
    EXPECT_DOUBLE_EQ(bars[i + 1].close_raw / kScale, expectedCloses[i]) << "brick " << i;
  }
}

// The other five aggregators never emit more than one bar per trade, so
// sizing bars_out to the trade count must remain exactly enough for them --
// this is the invariant the bindings still lean on for everything but Renko.
TEST(CapiBarAggregationTest, NonRenkoAggregatorsNeverExceedTradeCount)
{
  GapTape tape;
  const auto n = static_cast<uint32_t>(tape.ts.size());
  std::vector<FloxBar> bars(n);

  EXPECT_LE(flox_aggregate_time_bars(tape.ts.data(), tape.px.data(), tape.qty.data(), tape.isBuy.data(),
                                     tape.ts.size(), 1.0, bars.data(), n),
            n);
  EXPECT_LE(flox_aggregate_volume_bars(tape.ts.data(), tape.px.data(), tape.qty.data(), tape.isBuy.data(),
                                       tape.ts.size(), 0.5, bars.data(), n),
            n);
  EXPECT_LE(flox_aggregate_range_bars(tape.ts.data(), tape.px.data(), tape.qty.data(), tape.isBuy.data(),
                                      tape.ts.size(), 10.0, bars.data(), n),
            n);
}

// The C path runs its own copy of the close logic (doAggregateC), so the brick
// geometry has to be pinned here as well: brick size 10 over 100, 95, 111
// closes one brick, 100 -> 110, pointing up. The C aggregator emitted the bar
// before applying the crossing trade, so it reported 100 -> 95.
TEST(CapiBarAggregationTest, RenkoBrickClosesAtTheBrickBoundary)
{
  const std::vector<int64_t> ts{0, 1'000'000'000, 2'000'000'000};
  const std::vector<double> px{100.0, 95.0, 111.0};
  const std::vector<double> qty{1.0, 1.0, 1.0};
  const std::vector<uint8_t> isBuy{1, 1, 1};

  std::vector<FloxBar> bars(8);
  const uint32_t count = flox_aggregate_renko_bars(ts.data(), px.data(), qty.data(), isBuy.data(),
                                                   ts.size(), 10.0, bars.data(), 8);

  ASSERT_EQ(count, 1u);
  static constexpr double kScale = 1e8;
  EXPECT_DOUBLE_EQ(bars[0].open_raw / kScale, 100.0);
  EXPECT_DOUBLE_EQ(bars[0].close_raw / kScale, 110.0);
  EXPECT_GT(bars[0].close_raw, bars[0].open_raw) << "the move was up";
  EXPECT_EQ(bars[0].trade_count, 3u) << "the crossing trade belongs to the brick it completed";
}

// Two trades, brick size 0.01, a jump from 1.00 to 1000.00: 99,900 brick
// widths. doAggregateC bounds what it *writes* to bars_out, but the bricks are
// still all materialized -- gapBricks() builds a vector of them first -- so the
// work done by one call still scales with how far the price moved. The count
// the function reports is the number of bricks it produced, and it must be
// bounded by a constant.
TEST(CapiBarAggregationTest, RenkoGapIsBounded)
{
  const std::vector<int64_t> ts{0, 1'000'000'000};
  const std::vector<double> px{1.0, 1000.0};
  const std::vector<double> qty{1.0, 1.0};
  const std::vector<uint8_t> isBuy{1, 1};

  std::vector<FloxBar> bars(4);
  const uint32_t count = flox_aggregate_renko_bars(ts.data(), px.data(), qty.data(), isBuy.data(),
                                                   ts.size(), 0.01, bars.data(), 4);

  EXPECT_LE(count, 1024u) << "one trade produced " << count << " bricks";
}
