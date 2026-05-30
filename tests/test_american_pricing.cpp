#include <gtest/gtest.h>

#include "flox/pricing/american.h"
#include "flox/pricing/black_scholes.h"

#include <cmath>

using namespace flox;
using namespace flox::pricing;

// A positive-rate, zero-carry put has a real early-exercise premium: holding the
// option forgoes interest on the strike you'd collect by exercising now.
TEST(AmericanPricingTest, AmericanPutExceedsEuropean)
{
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.08, b = 0.0, vol = 0.30;
  const double amer = binomialPrice(OptionType::PUT, S, K, T, r, b, vol, 500, true);
  const double euro = bsPrice(OptionType::PUT, S, K, T, r, b, vol);
  EXPECT_GT(amer, euro);
  EXPECT_GT(amer, euro + 1e-3);  // premium is economically meaningful, not noise
}

// With no early-exercise check the lattice IS a European pricer and must
// converge to closed-form Black-Scholes as steps grow.
TEST(AmericanPricingTest, CrrConvergesToBlackScholes)
{
  const double S = 100.0, K = 105.0, T = 0.75, r = 0.05, b = 0.05, vol = 0.25;
  const double bs = bsPrice(OptionType::CALL, S, K, T, r, b, vol);

  const double coarse = std::fabs(binomialPrice(OptionType::CALL, S, K, T, r, b, vol, 20, false) - bs);
  const double fine = std::fabs(binomialPrice(OptionType::CALL, S, K, T, r, b, vol, 2000, false) - bs);
  EXPECT_LT(fine, coarse);  // refining the tree reduces the error
  EXPECT_LT(fine, 5e-3);    // and the fine tree is close in absolute terms
}

// An American call on an asset whose carry covers the rate (b >= r) is never
// exercised early, so BAW must reproduce the European price exactly.
TEST(AmericanPricingTest, BawCallNoCarryEqualsEuropean)
{
  const double S = 100.0, K = 95.0, T = 0.5, r = 0.05, b = 0.05, vol = 0.20;
  const double baw = bawPrice(OptionType::CALL, S, K, T, r, b, vol);
  const double euro = bsPrice(OptionType::CALL, S, K, T, r, b, vol);
  EXPECT_NEAR(baw, euro, 1e-9);
}

// BAW is an approximation of the true (binomial) American value. Across a grid
// of moneyness it should track a fine tree within a small tolerance.
TEST(AmericanPricingTest, BawTracksBinomialOnGrid)
{
  const double K = 100.0, T = 0.5, r = 0.10, b = -0.04, vol = 0.25;  // b<r -> call premium too
  for (double S : {80.0, 90.0, 100.0, 110.0, 120.0})
  {
    for (OptionType type : {OptionType::CALL, OptionType::PUT})
    {
      const double baw = bawPrice(type, S, K, T, r, b, vol);
      const double tree = binomialPrice(type, S, K, T, r, b, vol, 1500, true);
      EXPECT_NEAR(baw, tree, 0.10) << "S=" << S << " type=" << static_cast<int>(type);
      // BAW must never undervalue the European floor.
      EXPECT_GE(baw, bsPrice(type, S, K, T, r, b, vol) - 1e-6);
    }
  }
}

// Dispatch routes European to Black-Scholes and American to BAW transparently.
TEST(AmericanPricingTest, ExerciseStyleDispatch)
{
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.08, b = 0.0, vol = 0.30;

  const double euro = optionPrice(ExerciseStyle::European, OptionType::PUT, S, K, T, r, b, vol);
  EXPECT_DOUBLE_EQ(euro, bsPrice(OptionType::PUT, S, K, T, r, b, vol));

  const double amer = optionPrice(ExerciseStyle::American, OptionType::PUT, S, K, T, r, b, vol);
  EXPECT_DOUBLE_EQ(amer, bawPrice(OptionType::PUT, S, K, T, r, b, vol));
  EXPECT_GT(amer, euro);  // American put carries the early-exercise premium
}

// Degenerate inputs collapse to intrinsic, never NaN on a well-formed contract.
TEST(AmericanPricingTest, DegenerateInputsAreIntrinsic)
{
  EXPECT_DOUBLE_EQ(binomialPrice(OptionType::CALL, 110.0, 100.0, 0.0, 0.05, 0.0, 0.2, 100, true),
                   10.0);
  EXPECT_DOUBLE_EQ(bawPrice(OptionType::PUT, 90.0, 100.0, 0.0, 0.05, 0.0, 0.2), 10.0);
}
