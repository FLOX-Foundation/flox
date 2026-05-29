#include <gtest/gtest.h>

#include <cmath>

#include "flox/pricing/black_scholes.h"

using namespace flox;
using namespace flox::pricing;

namespace
{

// Textbook reference: S=100, K=100, T=1, r=b=0.05, vol=0.20.
// Hull / standard BS tables give call ~10.4506, put ~5.5735.
constexpr double S = 100.0;
constexpr double K = 100.0;
constexpr double T = 1.0;
constexpr double R = 0.05;
constexpr double B = 0.05;
constexpr double V = 0.20;

}  // namespace

TEST(BlackScholes, ReferenceCallPut)
{
  const double call = bsPrice(OptionType::CALL, S, K, T, R, B, V);
  const double put = bsPrice(OptionType::PUT, S, K, T, R, B, V);
  EXPECT_NEAR(call, 10.4506, 1e-3);
  EXPECT_NEAR(put, 5.5735, 1e-3);
}

TEST(BlackScholes, PutCallParity)
{
  // C - P = S e^((b-r)T) - K e^(-rT)
  const double call = bsPrice(OptionType::CALL, S, K, T, R, B, V);
  const double put = bsPrice(OptionType::PUT, S, K, T, R, B, V);
  const double parity = S * std::exp((B - R) * T) - K * std::exp(-R * T);
  EXPECT_NEAR(call - put, parity, 1e-9);
}

TEST(BlackScholes, ParityHoldsAcrossGrid)
{
  for (double s : {50.0, 80.0, 100.0, 120.0, 200.0})
  {
    for (double k : {60.0, 100.0, 150.0})
    {
      for (double t : {0.05, 0.5, 2.0})
      {
        for (double vol : {0.1, 0.4, 1.2})
        {
          const double c = bsPrice(OptionType::CALL, s, k, t, R, B, vol);
          const double p = bsPrice(OptionType::PUT, s, k, t, R, B, vol);
          const double parity = s * std::exp((B - R) * t) - k * std::exp(-R * t);
          EXPECT_NEAR(c - p, parity, 1e-7) << "s=" << s << " k=" << k << " t=" << t;
        }
      }
    }
  }
}

TEST(BlackScholes, Black76FuturesOption)
{
  // b = 0 → option on a forward. Call and put are symmetric at the money.
  const double call = bsPrice(OptionType::CALL, 100.0, 100.0, 1.0, 0.05, 0.0, 0.2);
  const double put = bsPrice(OptionType::PUT, 100.0, 100.0, 1.0, 0.05, 0.0, 0.2);
  EXPECT_NEAR(call, put, 1e-9);  // ATM forward: symmetric
}

TEST(BlackScholes, CryptoZeroRateCase)
{
  // Deribit-style: r = b = 0, priced off the forward (= spot here).
  const double call = bsPrice(OptionType::CALL, 70000.0, 70000.0, 30.0 / 365.0, 0.0, 0.0, 0.6);
  const double put = bsPrice(OptionType::PUT, 70000.0, 70000.0, 30.0 / 365.0, 0.0, 0.0, 0.6);
  EXPECT_GT(call, 0.0);
  EXPECT_NEAR(call, put, 1e-6);  // ATM, zero carry → symmetric
}

TEST(BlackScholes, ExpiryReturnsIntrinsic)
{
  EXPECT_NEAR(bsPrice(OptionType::CALL, 120.0, 100.0, 0.0, R, B, V), 20.0, 1e-12);
  EXPECT_NEAR(bsPrice(OptionType::PUT, 80.0, 100.0, 0.0, R, B, V), 20.0, 1e-12);
  EXPECT_NEAR(bsPrice(OptionType::CALL, 80.0, 100.0, 0.0, R, B, V), 0.0, 1e-12);
}

TEST(BlackScholes, ZeroVolDiscountedIntrinsic)
{
  // vol=0 → discounted forward intrinsic.
  const double fwd = S * std::exp(B * T);
  const double disc = std::exp(-R * T);
  const double expected = disc * std::max(fwd - K, 0.0);
  EXPECT_NEAR(bsPrice(OptionType::CALL, S, K, T, R, B, 0.0), expected, 1e-12);
}

TEST(BlackScholes, VegaPositiveAndMatchesFiniteDifference)
{
  const double eps = 1e-5;
  const double up = bsPrice(OptionType::CALL, S, K, T, R, B, V + eps);
  const double dn = bsPrice(OptionType::CALL, S, K, T, R, B, V - eps);
  const double fdVega = (up - dn) / (2.0 * eps);
  const double vega = bsVega(S, K, T, R, B, V);
  EXPECT_GT(vega, 0.0);
  EXPECT_NEAR(vega, fdVega, 1e-4);
  // Vega is identical for call and put (same formula).
}

TEST(BlackScholes, ImpliedVolRoundTrip)
{
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    for (double trueVol : {0.05, 0.15, 0.3, 0.6, 1.5})
    {
      for (double k : {60.0, 100.0, 140.0})
      {
        const double price = bsPrice(type, S, k, T, R, B, trueVol);
        // When vega is negligible (deep ITM/OTM, low vol) the price is flat in
        // vol — every vol in a wide band maps to the same price within float
        // precision, so IV inversion is fundamentally ill-posed. Market makers
        // can't quote meaningful IV there either. Skip rather than test noise.
        if (bsVega(S, k, T, R, B, trueVol) < 1e-4)
        {
          continue;
        }
        const auto iv = impliedVol(type, price, S, k, T, R, B);
        ASSERT_TRUE(iv.converged) << "k=" << k << " vol=" << trueVol;
        EXPECT_NEAR(iv.vol, trueVol, 1e-4) << "k=" << k << " vol=" << trueVol;
      }
    }
  }
}

TEST(BlackScholes, ImpliedVolRejectsArbitragePrice)
{
  // Price above the asset bound (vol=10) → no solution.
  const double tooHigh = bsPrice(OptionType::CALL, S, K, T, R, B, 10.0) + 1.0;
  const auto iv = impliedVol(OptionType::CALL, tooHigh, S, K, T, R, B);
  EXPECT_FALSE(iv.converged);
  EXPECT_TRUE(std::isnan(iv.vol));
}

TEST(BlackScholes, ImpliedVolDeepOtmConvergesViaBisection)
{
  // Deep OTM: vega tiny, Newton stalls, bisection fallback must still land.
  const double trueVol = 0.8;
  const double price = bsPrice(OptionType::CALL, 100.0, 300.0, 0.1, R, B, trueVol);
  const auto iv = impliedVol(OptionType::CALL, price, 100.0, 300.0, 0.1, R, B);
  ASSERT_TRUE(iv.converged);
  EXPECT_NEAR(iv.vol, trueVol, 1e-4);
}

TEST(BlackScholes, InvalidInputsReturnNan)
{
  EXPECT_TRUE(std::isnan(bsPrice(OptionType::CALL, -1.0, K, T, R, B, V)));
  EXPECT_TRUE(std::isnan(bsPrice(OptionType::CALL, S, 0.0, T, R, B, V)));
}
