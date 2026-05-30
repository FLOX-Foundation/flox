#include <gtest/gtest.h>

#include "flox/position/hedge_templates.h"

#include <cmath>
#include <vector>

using namespace flox;

namespace
{
// A saw-tooth spot path oscillating between lo and hi, `cycles` round trips,
// `perLeg` steps per leg, with time to expiry decaying slowly from t0. Large
// amplitude => high realized vol; pass a low mark vol to make the option cheap.
std::vector<HedgePathPoint> oscillatingPath(double lo, double hi, int cycles, int perLeg,
                                            double t0, double vol)
{
  std::vector<HedgePathPoint> path;
  const int legSteps = perLeg;
  double t = t0;
  const double dt = t0 * 0.5 / (cycles * 2 * legSteps);  // decay only half of t0 total
  bool up = true;
  double cur = lo;
  path.push_back({cur, t, vol});
  for (int c = 0; c < cycles * 2; ++c)
  {
    const double from = up ? lo : hi;
    const double to = up ? hi : lo;
    for (int s = 1; s <= legSteps; ++s)
    {
      cur = from + (to - from) * (static_cast<double>(s) / legSteps);
      t -= dt;
      path.push_back({cur, t, vol});
    }
    up = !up;
  }
  return path;
}
}  // namespace

// net() must equal the sum of the attributed legs minus the cost legs — the
// attribution is exhaustive, every dollar lands in exactly one bucket.
TEST(HedgeTemplatesTest, AttributionIdentity)
{
  const auto path = oscillatingPath(90.0, 110.0, 3, 5, 0.10, 0.30);
  const auto r = gammaScalp(OptionType::CALL, 100.0, 0.0, 0.0, 1.0, 0.05, 5.0, 1e-5, path);
  EXPECT_NEAR(r.net(), r.optionPnl + r.hedgePnl - r.transactionCost - r.fundingCost, 1e-9);
}

// Gamma scalping a path whose realized vol far exceeds the (cheap) implied vol
// it was marked at must turn a profit once friction is removed: the rehedge
// trades harvest the swings while the option's theta is tiny.
TEST(HedgeTemplatesTest, GammaScalpProfitsOnHighRealizedVol)
{
  const auto path = oscillatingPath(90.0, 110.0, 4, 6, 0.05, 0.10);  // 20% swings, 10% IV
  const auto r = gammaScalp(OptionType::CALL, 100.0, 0.0, 0.0, 1.0, 0.02,
                            /*costBps=*/0.0, /*funding=*/0.0, path);
  EXPECT_GT(r.net(), 0.0);
  EXPECT_GT(r.rebalances, 0u);
}

// A tighter band rehedges more often, so it pays more transaction cost — the
// same band/cost trade-off the cost-of-hedge optimizer is built around.
TEST(HedgeTemplatesTest, TighterBandCostsMore)
{
  const auto path = oscillatingPath(95.0, 105.0, 4, 5, 0.10, 0.30);
  const auto tight = gammaScalp(OptionType::CALL, 100.0, 0.0, 0.0, 1.0, 0.02, 5.0, 0.0, path);
  const auto wide = gammaScalp(OptionType::CALL, 100.0, 0.0, 0.0, 1.0, 0.30, 5.0, 0.0, path);
  EXPECT_GT(tight.rebalances, wide.rebalances);
  EXPECT_GT(tight.transactionCost, wide.transactionCost);
}

// On a crash, the long put gains and cushions the underlying's loss: the hedged
// book loses far less than the bare underlying leg.
TEST(HedgeTemplatesTest, ProtectivePutCapsDownside)
{
  // Spot slides 100 -> 60 over the horizon; ATM put struck at 100.
  std::vector<HedgePathPoint> path;
  const int n = 20;
  for (int i = 0; i <= n; ++i)
  {
    const double spot = 100.0 - 40.0 * (static_cast<double>(i) / n);
    const double t = 0.25 * (1.0 - static_cast<double>(i) / n) + 1e-4;
    path.push_back({spot, t, 0.50});
  }
  const auto r = protectivePut(/*strike=*/100.0, 0.0, 0.0, /*underlyingQty=*/1.0,
                               /*putQty=*/1.0, path);

  EXPECT_LT(r.hedgePnl, 0.0);                // underlying leg lost on the crash
  EXPECT_GT(r.optionPnl, 0.0);               // put leg gained
  EXPECT_GT(r.net(), r.hedgePnl);            // the put cushioned the loss
  EXPECT_DOUBLE_EQ(r.transactionCost, 0.0);  // static structure, no rehedging
  EXPECT_DOUBLE_EQ(r.fundingCost, 0.0);
}

// Funding is charged on the carried perp hedge notional; turning it on can only
// reduce net relative to the same run with funding off.
TEST(HedgeTemplatesTest, FundingReducesNet)
{
  const auto path = oscillatingPath(95.0, 105.0, 3, 5, 0.10, 0.30);
  const auto noFunding = gammaScalp(OptionType::CALL, 100.0, 0.0, 0.0, 1.0, 0.05, 5.0, 0.0, path);
  const auto withFunding = gammaScalp(OptionType::CALL, 100.0, 0.0, 0.0, 1.0, 0.05, 5.0, 1e-4, path);
  EXPECT_GT(withFunding.fundingCost, 0.0);
  EXPECT_LT(withFunding.net(), noFunding.net());
}

TEST(HedgeTemplatesTest, ShortPathIsZero)
{
  std::vector<HedgePathPoint> one{{100.0, 0.1, 0.3}};
  const auto r = gammaScalp(OptionType::CALL, 100.0, 0.0, 0.0, 1.0, 0.05, 5.0, 0.0, one);
  EXPECT_DOUBLE_EQ(r.net(), 0.0);
  EXPECT_EQ(r.rebalances, 0u);
}
