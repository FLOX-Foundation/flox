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

// At the crypto-default rate=0, carry=0: K = 1 - exp(-rate*t) and m = 2*rate/vol^2
// both vanish, and evaluating them separately made BAW divide 0/0 -> NaN, which
// then fell back to the strike as a fake critical price. A rate of exactly zero
// must price the same continuous family BAW handles everywhere else -- this is a
// grid analog of the repository's BawTracksBinomialOnGrid with r=0, b=0 substituted
// for the previous mutation-tested r=0.10, b=-0.04, which used to fail on every
// point in the grid (NaN or a 100% relative error against the tree).
TEST(AmericanPricingTest, BawTracksBinomialAtZeroRate)
{
  const double K = 100.0, T = 1.0, r = 0.0, b = 0.0, vol = 0.3;
  for (double S : {80.0, 90.0, 95.0, 100.0, 105.0, 110.0, 120.0})
  {
    for (OptionType type : {OptionType::CALL, OptionType::PUT})
    {
      const double baw = bawPrice(type, S, K, T, r, b, vol);
      const double tree = binomialPrice(type, S, K, T, r, b, vol, 1500, true);
      ASSERT_FALSE(std::isnan(baw)) << "S=" << S << " type=" << static_cast<int>(type);
      EXPECT_NEAR(baw, tree, 0.10) << "S=" << S << " type=" << static_cast<int>(type);
    }
  }
}

// A call is never worth exercising early when carry >= rate (holding costs
// nothing extra versus exercising, so there is no reason to give up optionality)
// -- bawPrice's own shortcut for this is line ~204. At rate=0 this covers every
// non-negative carry, including the crypto default (both zero), which is
// exactly the regime the K=1-exp(-rate*t) NaN used to corrupt.
TEST(AmericanPricingTest, ZeroRateCallEqualsEuropeanWhenCarryNonNegative)
{
  const double K = 100.0, T = 1.0, vol = 0.3;
  for (double b : {0.0, 0.05})
  {
    for (double S : {80.0, 100.0, 120.0})
    {
      const double baw = bawPrice(OptionType::CALL, S, K, T, 0.0, b, vol);
      const double euro = bsPrice(OptionType::CALL, S, K, T, 0.0, b, vol);
      ASSERT_FALSE(std::isnan(baw)) << "S=" << S << " b=" << b;
      EXPECT_NEAR(baw, euro, 1e-6) << "S=" << S << " b=" << b;
    }
  }
}

// By the mirror-image rule, a put is never worth exercising early when
// carry <= rate: the interest earned by collecting the strike now is offset (or
// worse) by the carry cost of holding the position instead. At rate=0 this
// covers every non-positive carry -- confirmed against an independent
// binomial tree, not just against bawPrice's own European branch, since bawPrice
// has no explicit put-side shortcut and must reach this answer through the
// general Newton-solved formula.
TEST(AmericanPricingTest, ZeroRatePutEqualsEuropeanWhenCarryNonPositive)
{
  const double K = 100.0, T = 1.0, vol = 0.3;
  for (double b : {-0.05, 0.0})
  {
    for (double S : {80.0, 100.0, 120.0})
    {
      const double baw = bawPrice(OptionType::PUT, S, K, T, 0.0, b, vol);
      const double euro = bsPrice(OptionType::PUT, S, K, T, 0.0, b, vol);
      const double tree = binomialPrice(OptionType::PUT, S, K, T, 0.0, b, vol, 2000, true);
      ASSERT_FALSE(std::isnan(baw)) << "S=" << S << " b=" << b;
      EXPECT_NEAR(baw, euro, 1e-4) << "S=" << S << " b=" << b;
      EXPECT_NEAR(baw, tree, 0.05) << "S=" << S << " b=" << b;
    }
  }
}

// The opposite carry sign is the genuine early-exercise regime for each type
// (call: carry < rate; put: carry > rate) -- BAW must diverge from the
// European price there and track the tree instead, so the two tests above are
// not vacuously true across all carries.
TEST(AmericanPricingTest, ZeroRateGenuineEarlyExerciseRegimeDivergesFromEuropean)
{
  const double K = 100.0, T = 1.0, S = 100.0, vol = 0.3;

  const double callBaw = bawPrice(OptionType::CALL, S, K, T, 0.0, -0.05, vol);
  const double callEuro = bsPrice(OptionType::CALL, S, K, T, 0.0, -0.05, vol);
  const double callTree = binomialPrice(OptionType::CALL, S, K, T, 0.0, -0.05, vol, 2000, true);
  EXPECT_GT(callBaw, callEuro + 0.1);
  EXPECT_NEAR(callBaw, callTree, 0.05);

  const double putBaw = bawPrice(OptionType::PUT, S, K, T, 0.0, 0.05, vol);
  const double putEuro = bsPrice(OptionType::PUT, S, K, T, 0.0, 0.05, vol);
  const double putTree = binomialPrice(OptionType::PUT, S, K, T, 0.0, 0.05, vol, 2000, true);
  EXPECT_GT(putBaw, putEuro + 0.1);
  EXPECT_NEAR(putBaw, putTree, 0.1);
}

// BAW must vary continuously as rate -> 0, not jump from a real price to NaN
// exactly at the boundary.
TEST(AmericanPricingTest, ContinuousAsRateApproachesZero)
{
  const double K = 100.0, S = 100.0, T = 1.0, b = 0.0, vol = 0.3;
  const double atZero = bawPrice(OptionType::PUT, S, K, T, 0.0, b, vol);
  const double nearZero = bawPrice(OptionType::PUT, S, K, T, 1e-8, b, vol);
  ASSERT_FALSE(std::isnan(atZero));
  ASSERT_FALSE(std::isnan(nearZero));
  EXPECT_NEAR(atZero, nearZero, 1e-4);
}

// The public bawCriticalPrice wrapper must agree with bawPrice's own internal
// judgment about when early exercise is never optimal: bawPrice short-circuits a
// call to the European price whenever carry >= rate (line ~204), so the critical
// price the wrapper reports for that same call must be unreachable (+inf), not a
// finite spot -- otherwise a caller like OptionExerciseEngine which only sees the
// wrapper (not bawPrice's internal guard) exercises calls that are never actually
// optimal to exercise.
TEST(AmericanPricingTest, CriticalPriceUnreachableWhenCarryCoversRate)
{
  // b == r: at the crypto default (both zero) the unguarded quadratic formula
  // used to divide 0/0 and fall back to `strike` -- exactly reachable and wrong.
  EXPECT_TRUE(std::isinf(bawCriticalPrice(OptionType::CALL, 100.0, 1.0, 0.0, 0.0, 0.3)));
  EXPECT_TRUE(std::isinf(bawCriticalPrice(OptionType::CALL, 100.0, 1.0, 0.05, 0.05, 0.2)));
  // b > r: the unguarded formula lands on a finite, spot-reachable number.
  EXPECT_TRUE(std::isinf(bawCriticalPrice(OptionType::CALL, 100.0, 1.0, 0.05, 0.08, 0.3)));

  // b < r is the genuine early-exercise regime for a call: the critical price
  // must stay finite and unaffected by the guard.
  const double crit = bawCriticalPrice(OptionType::CALL, 100.0, 0.5, 0.05, 0.0, 0.25);
  EXPECT_TRUE(std::isfinite(crit));
  EXPECT_GT(crit, 100.0);

  // A put has no such "never optimal" shortcut; the crypto-default critical
  // price must be a genuine, finite, reachable boundary, not the strike itself
  // (which was the NaN-fallback value before the fix).
  const double critPut = bawCriticalPrice(OptionType::PUT, 100.0, 1.0, 0.0, 0.0, 0.3);
  EXPECT_TRUE(std::isfinite(critPut));
  EXPECT_LT(critPut, 100.0);
}
