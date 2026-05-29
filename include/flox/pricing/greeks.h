#pragma once

#include "flox/common.h"
#include "flox/pricing/black_scholes.h"

#include <cmath>

// Analytic greeks for the generalized Black-Scholes-Merton model (cost-of-carry
// form, see black_scholes.h). Same carry convention b:
//   b = r (stock), b = r - q (dividend), b = 0 (future/Black-76), b = r - rf (FX).
//
// Conventions:
//   - delta: per 1.0 move in spot
//   - gamma: per 1.0^2 move in spot
//   - vega:  per 1.0 (=100 vol points) change in vol; divide by 100 for per-vol-point
//   - theta: per YEAR; divide by 365 for per-calendar-day
//   - rho:   per 1.0 (=100%) change in discount rate r, holding carry b FIXED
//            (the unambiguous partial dV/dr in the generalized model). For
//            plain-stock semantics where b=r, add the carry sensitivity if you
//            want r and b to move together. Divide by 100 for per-1%-rate.

namespace flox::pricing
{

struct Greeks
{
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;  // per year
  double rho = 0.0;
};

struct SecondOrderGreeks
{
  double vanna = 0.0;  // d(delta)/d(vol) = d(vega)/d(spot)
  double volga = 0.0;  // d(vega)/d(vol)  (a.k.a. vomma)
  double charm = 0.0;  // d(delta)/d(time), per year
};

// First-order greeks. Degenerate inputs (t<=0 or vol<=0) return zeros except a
// best-effort delta of the discounted intrinsic boundary.
inline Greeks greeks(OptionType type, double spot, double strike, double t, double rate,
                     double carry, double vol) noexcept
{
  Greeks g;
  if (spot <= 0.0 || strike <= 0.0 || t <= 0.0 || vol <= 0.0)
  {
    return g;
  }

  const double sqrtT = std::sqrt(t);
  const double d1 = bsD1(spot, strike, t, carry, vol);
  const double d2 = d1 - vol * sqrtT;
  const double carryDisc = std::exp((carry - rate) * t);
  const double rateDisc = std::exp(-rate * t);
  const double pdf = normPdf(d1);

  if (type == OptionType::CALL)
  {
    g.delta = carryDisc * normCdf(d1);
    // b-fixed partial dV/dr: discount term plus the carry-discount term.
    g.rho = strike * t * rateDisc * normCdf(d2) - t * spot * carryDisc * normCdf(d1);
    g.theta = -spot * carryDisc * pdf * vol / (2.0 * sqrtT) -
              (carry - rate) * spot * carryDisc * normCdf(d1) -
              rate * strike * rateDisc * normCdf(d2);
  }
  else
  {
    g.delta = -carryDisc * normCdf(-d1);
    g.rho = -strike * t * rateDisc * normCdf(-d2) + t * spot * carryDisc * normCdf(-d1);
    g.theta = -spot * carryDisc * pdf * vol / (2.0 * sqrtT) +
              (carry - rate) * spot * carryDisc * normCdf(-d1) +
              rate * strike * rateDisc * normCdf(-d2);
  }

  g.gamma = carryDisc * pdf / (spot * vol * sqrtT);
  g.vega = spot * carryDisc * pdf * sqrtT;
  return g;
}

// Second-order greeks. Vanna and volga are call/put symmetric; charm differs by
// sign of the carry-drift term.
inline SecondOrderGreeks secondOrderGreeks(OptionType type, double spot, double strike,
                                           double t, double rate, double carry,
                                           double vol) noexcept
{
  SecondOrderGreeks s;
  if (spot <= 0.0 || strike <= 0.0 || t <= 0.0 || vol <= 0.0)
  {
    return s;
  }

  const double sqrtT = std::sqrt(t);
  const double d1 = bsD1(spot, strike, t, carry, vol);
  const double d2 = d1 - vol * sqrtT;
  const double carryDisc = std::exp((carry - rate) * t);
  const double pdf = normPdf(d1);
  const double vega = spot * carryDisc * pdf * sqrtT;

  // vanna = d(vega)/d(spot) = -e^((b-r)T) phi(d1) d2 / vol
  s.vanna = -carryDisc * pdf * d2 / vol;
  // volga = vega * d1 * d2 / vol
  s.volga = vega * d1 * d2 / vol;

  // charm = d(delta)/d(t). Haug generalized form.
  const double common = carryDisc * pdf * (2.0 * carry * t - d2 * vol * sqrtT) /
                        (2.0 * t * vol * sqrtT);
  if (type == OptionType::CALL)
  {
    s.charm = -common + (carry - rate) * carryDisc * normCdf(d1);
  }
  else
  {
    s.charm = -common - (carry - rate) * carryDisc * normCdf(-d1);
  }
  return s;
}

}  // namespace flox::pricing
