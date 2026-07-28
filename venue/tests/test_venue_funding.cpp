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
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
}  // namespace

TEST(VenueFunding, EngineSuite)
{
  std::printf("test_funding\n");
  FundingCalculator fc;  // defaults: interest 1bp, band 5bp, cap 0.75%

  CHECK(fc.rate(101, 100) > 0);            // mark above index -> longs pay
  CHECK(fc.rate(99, 100) < 0);             // mark below index -> shorts pay
  CHECK(near(fc.rate(100, 100), 0.0001));  // premium 0 -> interest component
  CHECK(near(fc.rate(200, 100), 0.0075));  // huge premium -> capped
  CHECK(near(fc.rate(1, 100), -0.0075));   // huge negative -> capped
  CHECK(near(fc.rate(100, 0), 0.0));       // no index -> no funding

  // Settlement: balanced long/short book nets to zero; long pays when rate > 0.
  std::vector<PerpPosition> pos = {{1, +10.0}, {2, -10.0}};
  const auto pays = fc.settle(pos, 101.0, 100.0);
  CHECK(pays.size() == 2);
  CHECK(pays[0].account == 1 && pays[0].amount < 0);  // long pays
  CHECK(pays[1].account == 2 && pays[1].amount > 0);  // short receives
  CHECK(near(pays[0].amount + pays[1].amount, 0.0));  // zero-sum

  // --- interval TWAP (manipulation-resistant) ---
  // A single manipulated snapshot swings the naive rate to the cap; the interval
  // average over many honest samples dilutes the spike back toward the floor.
  FundingCalculator tw;
  for (int i = 0; i < 1000; ++i)
  {
    tw.sample(100.0, 100.0);  // 1000 honest samples @ premium 0
  }
  tw.sample(200.0, 100.0);  // one manipulative print (premium +100%)
  CHECK(tw.sampleCount() == 1001);
  const double snapRate = tw.rate(200.0, 100.0);  // naive single snapshot -> capped
  const double twapRate = tw.intervalRate();      // averaged -> near the floor
  CHECK(near(snapRate, 0.0075));                  // capped
  CHECK(twapRate < 0.002);                        // spike diluted, nowhere near the cap
  CHECK(twapRate < snapRate);

  // A sustained premium (every sample above index) produces a real positive rate.
  FundingCalculator sus;
  for (int i = 0; i < 50; ++i)
  {
    sus.sample(100.5, 100.0);  // steady +0.5% premium
  }
  CHECK(sus.intervalRate() > 0.004);  // meaningful positive funding

  // Reset clears the accumulator back to the interest floor.
  sus.resetSamples();
  CHECK(sus.sampleCount() == 0);
  CHECK(near(sus.intervalRate(), 0.0001));

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
