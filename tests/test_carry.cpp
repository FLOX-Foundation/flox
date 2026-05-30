#include <gtest/gtest.h>

#include "flox/pricing/black_scholes.h"
#include "flox/pricing/carry.h"

#include <vector>

using namespace flox;
using namespace flox::pricing;

// Cost-of-carry assembles from rate, dividend yield and borrow. Crypto (all
// zero) gives b = 0; equity nets the financing pieces.
TEST(CarryTest, CostOfCarryComposes)
{
  EXPECT_DOUBLE_EQ(costOfCarry(0.0, 0.0, 0.0), 0.0);      // crypto / Black-76
  EXPECT_DOUBLE_EQ(costOfCarry(0.05, 0.0, 0.0), 0.05);    // non-dividend stock: b = r
  EXPECT_DOUBLE_EQ(costOfCarry(0.05, 0.02, 0.0), 0.03);   // 2% dividend yield
  EXPECT_DOUBLE_EQ(costOfCarry(0.05, 0.02, 0.01), 0.02);  // plus 1% borrow
}

// Only dividends paid before expiry count, and they are discounted to present
// value. A dividend at or after expiry is ignored.
TEST(CarryTest, PresentValueOfDividends)
{
  const std::vector<Dividend> divs = {{0.25, 1.0}, {0.75, 1.0}, {1.5, 1.0}};
  const double rate = 0.04;
  const double t = 1.0;
  const double expected = 1.0 * std::exp(-rate * 0.25) + 1.0 * std::exp(-rate * 0.75);
  EXPECT_NEAR(presentValueOfDividends(divs, rate, t), expected, 1e-12);

  // No dividends before a very short expiry.
  EXPECT_DOUBLE_EQ(presentValueOfDividends(divs, rate, 0.1), 0.0);
}

// A discrete dividend lowers a call (holder forgoes the payout) and lifts a put,
// relative to the same option on a non-dividend stock.
TEST(CarryTest, DiscreteDividendShiftsCallAndPut)
{
  const double S = 100.0, K = 100.0, t = 1.0, r = 0.05, vol = 0.25;
  const std::vector<Dividend> divs = {{0.5, 3.0}};

  const double callNoDiv = bsPrice(OptionType::CALL, S, K, t, r, /*carry=*/r, vol);
  const double putNoDiv = bsPrice(OptionType::PUT, S, K, t, r, /*carry=*/r, vol);
  const double callDiv = bsPriceDiscreteDividends(OptionType::CALL, S, K, t, r, vol, divs);
  const double putDiv = bsPriceDiscreteDividends(OptionType::PUT, S, K, t, r, vol, divs);

  EXPECT_LT(callDiv, callNoDiv);
  EXPECT_GT(putDiv, putNoDiv);
}

// A dividend paid after expiry must not change the price.
TEST(CarryTest, DividendAfterExpiryIgnored)
{
  const double S = 100.0, K = 100.0, t = 0.5, r = 0.05, vol = 0.25;
  const std::vector<Dividend> after = {{0.9, 5.0}};
  const double withLate = bsPriceDiscreteDividends(OptionType::CALL, S, K, t, r, vol, after);
  const double none = bsPrice(OptionType::CALL, S, K, t, r, /*carry=*/r, vol);
  EXPECT_DOUBLE_EQ(withLate, none);
}

// The escrowed-dividend price matches a continuous-yield carry in the limit: a
// single dividend approximating a yield q over the life lands close to the
// generalized BSM priced at b = r - q.
TEST(CarryTest, ApproachesContinuousYield)
{
  const double S = 100.0, K = 100.0, t = 1.0, r = 0.05, vol = 0.20;
  const double q = 0.03;

  // Continuous-yield reference.
  const double cont = bsPrice(OptionType::CALL, S, K, t, r, costOfCarry(r, q), vol);

  // Approximate the yield with many small dividends spread across the year.
  std::vector<Dividend> divs;
  const int n = 200;
  for (int i = 1; i <= n; ++i)
  {
    const double ti = t * (static_cast<double>(i) - 0.5) / n;
    divs.push_back({ti, S * q * t / n});  // total ~ S*q*t paid in even slices
  }
  const double discrete = bsPriceDiscreteDividends(OptionType::CALL, S, K, t, r, vol, divs);
  EXPECT_NEAR(discrete, cont, 0.20);  // close, not exact (escrowed vs yield)
}
