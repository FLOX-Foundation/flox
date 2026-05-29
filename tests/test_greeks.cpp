#include <gtest/gtest.h>

#include <cmath>

#include "flox/pricing/black_scholes.h"
#include "flox/pricing/greeks.h"

using namespace flox;
using namespace flox::pricing;

namespace
{
constexpr double S = 100.0;
constexpr double K = 100.0;
constexpr double T = 1.0;
constexpr double R = 0.05;
constexpr double B = 0.05;
constexpr double V = 0.20;

double price(OptionType t, double s, double k, double tt, double r, double b, double v)
{
  return bsPrice(t, s, k, tt, r, b, v);
}
}  // namespace

TEST(Greeks, DeltaMatchesFiniteDifference)
{
  const double h = 1e-4;
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    const double fd = (price(type, S + h, K, T, R, B, V) - price(type, S - h, K, T, R, B, V)) /
                      (2.0 * h);
    EXPECT_NEAR(greeks(type, S, K, T, R, B, V).delta, fd, 1e-6);
  }
}

TEST(Greeks, GammaMatchesFiniteDifference)
{
  const double h = 1e-2;
  // gamma is call/put identical
  const double fd = (price(OptionType::CALL, S + h, K, T, R, B, V) -
                     2.0 * price(OptionType::CALL, S, K, T, R, B, V) +
                     price(OptionType::CALL, S - h, K, T, R, B, V)) /
                    (h * h);
  EXPECT_NEAR(greeks(OptionType::CALL, S, K, T, R, B, V).gamma, fd, 1e-5);
  EXPECT_NEAR(greeks(OptionType::PUT, S, K, T, R, B, V).gamma,
              greeks(OptionType::CALL, S, K, T, R, B, V).gamma, 1e-12);
}

TEST(Greeks, VegaMatchesFiniteDifference)
{
  const double h = 1e-5;
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    const double fd = (price(type, S, K, T, R, B, V + h) - price(type, S, K, T, R, B, V - h)) /
                      (2.0 * h);
    EXPECT_NEAR(greeks(type, S, K, T, R, B, V).vega, fd, 1e-3);
  }
}

TEST(Greeks, ThetaMatchesFiniteDifference)
{
  const double h = 1e-5;
  // theta = -dV/dtau (value as calendar time advances, tau shrinks)
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    const double fd = -(price(type, S, K, T + h, R, B, V) - price(type, S, K, T - h, R, B, V)) /
                      (2.0 * h);
    EXPECT_NEAR(greeks(type, S, K, T, R, B, V).theta, fd, 1e-3);
  }
}

TEST(Greeks, RhoMatchesFiniteDifference)
{
  const double h = 1e-6;
  // rho holds carry b fixed, bumps discount rate r only.
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    const double fd = (price(type, S, K, T, R + h, B, V) - price(type, S, K, T, R - h, B, V)) /
                      (2.0 * h);
    EXPECT_NEAR(greeks(type, S, K, T, R, B, V).rho, fd, 1e-2);
  }
}

TEST(Greeks, LongCallThetaNegative)
{
  EXPECT_LT(greeks(OptionType::CALL, S, K, T, R, B, V).theta, 0.0);
}

TEST(Greeks, DeltaBoundsAndSigns)
{
  const double dCall = greeks(OptionType::CALL, S, K, T, R, B, V).delta;
  const double dPut = greeks(OptionType::PUT, S, K, T, R, B, V).delta;
  EXPECT_GT(dCall, 0.0);
  EXPECT_LT(dCall, 1.5);  // e^((b-r)T)*N(d1), b=r → in [0,1]
  EXPECT_LT(dPut, 0.0);
}

TEST(SecondOrderGreeks, VannaMatchesFiniteDifference)
{
  const double h = 1e-5;
  // vanna = d(delta)/d(vol)
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    const double fd = (greeks(type, S, K, T, R, B, V + h).delta -
                       greeks(type, S, K, T, R, B, V - h).delta) /
                      (2.0 * h);
    EXPECT_NEAR(secondOrderGreeks(type, S, K, T, R, B, V).vanna, fd, 1e-3);
  }
}

TEST(SecondOrderGreeks, VolgaMatchesFiniteDifference)
{
  const double h = 1e-5;
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    const double fd = (greeks(type, S, K, T, R, B, V + h).vega -
                       greeks(type, S, K, T, R, B, V - h).vega) /
                      (2.0 * h);
    EXPECT_NEAR(secondOrderGreeks(type, S, K, T, R, B, V).volga, fd, 1e-2);
  }
}

TEST(SecondOrderGreeks, CharmMatchesFiniteDifference)
{
  const double h = 1e-6;
  // charm = d(delta)/d(calendar) = -d(delta)/d(tau)
  for (auto type : {OptionType::CALL, OptionType::PUT})
  {
    const double fd = -(greeks(type, S, K, T + h, R, B, V).delta -
                        greeks(type, S, K, T - h, R, B, V).delta) /
                      (2.0 * h);
    EXPECT_NEAR(secondOrderGreeks(type, S, K, T, R, B, V).charm, fd, 1e-2);
  }
}

TEST(Greeks, CryptoCaseGridFiniteDifference)
{
  // Deribit-style r=b=0, validate delta/gamma/vega FD across a small grid.
  const double r = 0.0, b = 0.0;
  for (double s : {60000.0, 70000.0, 80000.0})
  {
    for (double vol : {0.4, 0.8})
    {
      const double tt = 30.0 / 365.0;
      const double h = s * 1e-5;
      const double fdDelta =
          (price(OptionType::CALL, s + h, 70000.0, tt, r, b, vol) -
           price(OptionType::CALL, s - h, 70000.0, tt, r, b, vol)) /
          (2.0 * h);
      EXPECT_NEAR(greeks(OptionType::CALL, s, 70000.0, tt, r, b, vol).delta, fdDelta, 1e-4)
          << "s=" << s << " vol=" << vol;
    }
  }
}
