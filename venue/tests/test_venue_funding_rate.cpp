/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-venue/funding_rate.h"

#include <gtest/gtest.h>

using namespace flox;
using namespace flox::venue;

TEST(FundingRate, PremiumDrivesSignMarkAboveIndexIsPositive)
{
  FundingCalculator f;  // interest 1bp, clamp 5bp, cap 0.75%
  // mark > index: longs pay (positive rate); mark < index: shorts pay.
  EXPECT_GT(f.rate(/*mark=*/101.0, /*index=*/100.0), 0.0);
  EXPECT_LT(f.rate(/*mark=*/99.0, /*index=*/100.0), 0.0);
  // Flat market: rate collapses to the interest component.
  EXPECT_NEAR(f.rate(100.0, 100.0), 0.0001, 1e-12);
}

TEST(FundingRate, CapBoundsExtremeDislocation)
{
  FundingCalculator f;
  // A 50% premium must still clamp to the ±0.75% cap.
  EXPECT_NEAR(f.rate(150.0, 100.0), 0.0075, 1e-12);
  EXPECT_NEAR(f.rate(50.0, 100.0), -0.0075, 1e-12);
}

TEST(FundingRate, InvalidIndexYieldsZero)
{
  FundingCalculator f;
  EXPECT_DOUBLE_EQ(f.rate(100.0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(f.rate(100.0, -1.0), 0.0);
}

// The TWAP path is the manipulation-resistant one: a single dislocated print
// must not swing the settled rate the way a snapshot would.
TEST(FundingRate, IntervalTwapResistsSingleSpike)
{
  FundingCalculator f;
  for (int i = 0; i < 99; ++i)
  {
    f.sample(100.0, 100.0);  // flat
  }
  f.sample(150.0, 100.0);  // one 50% spike
  EXPECT_EQ(f.sampleCount(), 100u);

  const double twap = f.intervalRate();
  const double snapshot = f.rate(150.0, 100.0);
  EXPECT_LT(twap, snapshot);  // averaged away, not the capped snapshot
  // avg premium = 0.5% -> clamp(interest 0.01% - 0.5%, ±0.05%) = -0.05%
  // -> rate = 0.5% - 0.05% = 0.45% (below the 0.75% cap the snapshot hits).
  EXPECT_NEAR(twap, 0.0045, 1e-9);

  f.resetSamples();
  EXPECT_EQ(f.sampleCount(), 0u);
  // No samples -> falls back to the interest-only rate.
  EXPECT_NEAR(f.intervalRate(), 0.0001, 1e-12);
}

// Funding is a transfer between longs and shorts: on a balanced book the
// charges must net to zero (no money created by the settlement).
TEST(FundingRate, SettleIsZeroSumOnBalancedBook)
{
  FundingCalculator f;
  const std::vector<PerpPosition> pos{{/*account=*/1, +10.0}, {/*account=*/2, -4.0}, {/*account=*/3, -6.0}};
  const auto charges = f.settle(pos, /*mark=*/101.0, /*index=*/100.0);
  ASSERT_EQ(charges.size(), 3u);

  double sum = 0.0;
  for (const auto& c : charges)
  {
    sum += c.amount;
  }
  EXPECT_NEAR(sum, 0.0, 1e-9);        // zero-sum across the book
  EXPECT_LT(charges[0].amount, 0.0);  // long pays when mark > index
  EXPECT_GT(charges[1].amount, 0.0);  // shorts receive
}
