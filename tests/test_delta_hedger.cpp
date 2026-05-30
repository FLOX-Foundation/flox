#include <gtest/gtest.h>

#include "flox/position/delta_hedger.h"

using namespace flox;

TEST(DeltaHedgerTest, WithinBandNoTrade)
{
  DeltaHedger h(0.10);  // 0.10 delta band
  auto adj = h.step(0.05);
  EXPECT_FALSE(adj.rebalanced);
  EXPECT_DOUBLE_EQ(adj.quantity, 0.0);
  EXPECT_DOUBLE_EQ(h.hedgePosition(), 0.0);
  EXPECT_EQ(h.rebalanceCount(), 0u);
}

TEST(DeltaHedgerTest, OutsideBandNeutralizes)
{
  DeltaHedger h(0.10);
  // Net delta drifted to +0.6 -> short 0.6 perp units to neutralize.
  auto adj = h.step(0.6);
  EXPECT_TRUE(adj.rebalanced);
  EXPECT_DOUBLE_EQ(adj.quantity, -0.6);
  EXPECT_DOUBLE_EQ(h.hedgePosition(), -0.6);
  EXPECT_EQ(h.rebalanceCount(), 1u);
}

TEST(DeltaHedgerTest, NegativeDeltaBuysHedge)
{
  DeltaHedger h(0.10);
  auto adj = h.step(-0.45);  // short delta -> buy 0.45 to neutralize
  EXPECT_TRUE(adj.rebalanced);
  EXPECT_DOUBLE_EQ(adj.quantity, 0.45);
  EXPECT_DOUBLE_EQ(h.hedgePosition(), 0.45);
}

TEST(DeltaHedgerTest, HedgeInstrumentDeltaScalesTrade)
{
  DeltaHedger h(0.10, /*hedgeInstrumentDelta=*/0.5);  // hedging with a 0.5-delta option
  auto adj = h.step(1.0);
  // Need to offset +1.0 delta with a 0.5-delta instrument -> 2 units short.
  EXPECT_DOUBLE_EQ(adj.quantity, -2.0);
}

TEST(DeltaHedgerTest, AccumulatesAcrossStepsAndTracksTurnover)
{
  DeltaHedger h(0.10);
  h.step(0.5);   // short 0.5  -> pos -0.5, turnover 0.5
  h.step(0.3);   // short 0.3  -> pos -0.8, turnover 0.8
  h.step(-0.4);  // buy 0.4    -> pos -0.4, turnover 1.2
  h.step(0.05);  // within band -> no trade
  EXPECT_NEAR(h.hedgePosition(), -0.4, 1e-12);
  EXPECT_EQ(h.rebalanceCount(), 3u);
  EXPECT_NEAR(h.turnover(), 1.2, 1e-12);
}

TEST(DeltaHedgerTest, WiderBandRebalancesLess)
{
  DeltaHedger tight(0.05);
  DeltaHedger wide(0.50);
  for (double d : {0.1, 0.2, 0.3, -0.1, 0.4})
  {
    tight.step(d);
    wide.step(d);
  }
  EXPECT_GT(tight.rebalanceCount(), wide.rebalanceCount());
  EXPECT_GT(tight.turnover(), wide.turnover());
}

TEST(DeltaHedgerTest, ResetClears)
{
  DeltaHedger h(0.10);
  h.step(0.5);
  h.reset();
  EXPECT_DOUBLE_EQ(h.hedgePosition(), 0.0);
  EXPECT_EQ(h.rebalanceCount(), 0u);
  EXPECT_DOUBLE_EQ(h.turnover(), 0.0);
}
