#pragma once

#include "flox/common.h"
#include "flox/pricing/black_scholes.h"

#include <cmath>
#include <limits>
#include <vector>

// American option pricing. US equity options carry an early-exercise right that
// the European Black-Scholes (black_scholes.h) cannot value. Two engines:
//   - CRR binomial tree: a Cox-Ross-Rubinstein lattice with an early-exercise
//     check at every node. Converges to Black-Scholes on European parameters as
//     steps grow, and prices the American premium exactly in the limit.
//   - Barone-Adesi-Whaley: a closed-form quadratic approximation of the American
//     premium. Far cheaper than a fine tree, so it is the engine of choice for
//     greeks via finite differences.
// Both use the generalized cost-of-carry b (see black_scholes.h), so they cover
// equity (b=r-q), futures (b=0) and FX (b=r-rf), not just non-dividend stock.

namespace flox::pricing
{

// CRR binomial price. With american=false this is a European lattice that
// converges to bsPrice as steps grows (used to validate the tree); with
// american=true it takes max(continuation, intrinsic) at every node, capturing
// the early-exercise premium. steps must be >= 1.
inline double binomialPrice(OptionType type, double spot, double strike, double t, double rate,
                            double carry, double vol, int steps, bool american) noexcept
{
  if (spot <= 0.0 || strike <= 0.0)
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (steps < 1)
  {
    steps = 1;
  }
  if (t <= 0.0 || vol <= 0.0)
  {
    const double fwd = spot * std::exp(carry * t);
    return std::exp(-rate * t) * intrinsic(type, fwd, strike);
  }

  const double dt = t / steps;
  const double u = std::exp(vol * std::sqrt(dt));
  const double d = 1.0 / u;
  const double disc = std::exp(-rate * dt);
  // Risk-neutral up-probability under cost-of-carry b. Clamp to [0,1] to stay
  // well-defined if a coarse step makes the raw value drift slightly outside.
  double p = (std::exp(carry * dt) - d) / (u - d);
  if (p < 0.0)
  {
    p = 0.0;
  }
  else if (p > 1.0)
  {
    p = 1.0;
  }

  // Terminal payoffs at expiry: spot * u^(2j-steps).
  std::vector<double> v(static_cast<size_t>(steps) + 1);
  for (int j = 0; j <= steps; ++j)
  {
    const double sT = spot * std::pow(u, 2 * j - steps);
    v[static_cast<size_t>(j)] = intrinsic(type, sT, strike);
  }

  // Backward induction. At step i the node j has spot * u^(2j-i).
  for (int i = steps - 1; i >= 0; --i)
  {
    for (int j = 0; j <= i; ++j)
    {
      double cont = disc * (p * v[static_cast<size_t>(j) + 1] + (1.0 - p) * v[static_cast<size_t>(j)]);
      if (american)
      {
        const double sNode = spot * std::pow(u, 2 * j - i);
        cont = std::max(cont, intrinsic(type, sNode, strike));
      }
      v[static_cast<size_t>(j)] = cont;
    }
  }
  return v[0];
}

namespace detail
{

// Newton solve for the critical spot price S* above which an American call (or
// below which an American put) is exercised immediately. Follows Barone-Adesi &
// Whaley (1987) via Haug's formulation.
inline double bawCriticalPrice(OptionType type, double strike, double t, double rate, double carry,
                               double vol) noexcept
{
  const double v2 = vol * vol;
  const double nn = 2.0 * carry / v2;
  const double m = 2.0 * rate / v2;
  const double K = 1.0 - std::exp(-rate * t);
  const double sqrtT = std::sqrt(t);

  // Seed from the perpetual-option critical price, then damp toward the strike.
  const double q_u = type == OptionType::CALL
                         ? (-(nn - 1.0) + std::sqrt((nn - 1.0) * (nn - 1.0) + 4.0 * m)) / 2.0
                         : (-(nn - 1.0) - std::sqrt((nn - 1.0) * (nn - 1.0) + 4.0 * m)) / 2.0;
  const double s_u = strike / (1.0 - 1.0 / q_u);
  const double h = type == OptionType::CALL
                       ? -(carry * t + 2.0 * vol * sqrtT) * strike / (s_u - strike)
                       : (carry * t - 2.0 * vol * sqrtT) * strike / (strike - s_u);
  double si = type == OptionType::CALL ? strike + (s_u - strike) * (1.0 - std::exp(h))
                                       : s_u + (strike - s_u) * std::exp(h);
  if (!(si > 0.0))
  {
    si = strike;
  }

  const double q = type == OptionType::CALL
                       ? (-(nn - 1.0) + std::sqrt((nn - 1.0) * (nn - 1.0) + 4.0 * m / K)) / 2.0
                       : (-(nn - 1.0) - std::sqrt((nn - 1.0) * (nn - 1.0) + 4.0 * m / K)) / 2.0;

  for (int i = 0; i < 100; ++i)
  {
    const double d1 = bsD1(si, strike, t, carry, vol);
    const double euro = bsPrice(type, si, strike, t, rate, carry, vol);
    const double carryDisc = std::exp((carry - rate) * t);
    if (type == OptionType::CALL)
    {
      const double bias = (1.0 - carryDisc * normCdf(d1)) * si / q;
      const double lhs = euro + bias;
      const double rhs = si - strike;
      const double b_i = carryDisc * normCdf(d1) * (1.0 - 1.0 / q) +
                         (1.0 - carryDisc * normPdf(d1) / (vol * sqrtT)) / q;
      const double next = (strike + lhs - b_i * si) / (1.0 - b_i);
      if (std::fabs(lhs - rhs) / strike < 1e-6)
      {
        return si;
      }
      si = next;
    }
    else
    {
      const double bias = -(1.0 - carryDisc * normCdf(-d1)) * si / q;
      const double lhs = euro + bias;
      const double rhs = strike - si;
      const double b_i = -carryDisc * normCdf(-d1) * (1.0 - 1.0 / q) -
                         (1.0 + carryDisc * normPdf(-d1) / (vol * sqrtT)) / q;
      const double next = (strike - lhs + b_i * si) / (1.0 + b_i);
      if (std::fabs(lhs - rhs) / strike < 1e-6)
      {
        return si;
      }
      si = next;
    }
    if (!(si > 0.0))
    {
      si = strike;
    }
  }
  return si;
}

}  // namespace detail

// Barone-Adesi-Whaley American price: the European value plus a closed-form
// early-exercise premium. An American call on an asset with carry b >= rate is
// never exercised early, so it collapses to the European price exactly.
inline double bawPrice(OptionType type, double spot, double strike, double t, double rate,
                       double carry, double vol) noexcept
{
  if (spot <= 0.0 || strike <= 0.0)
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (t <= 0.0 || vol <= 0.0)
  {
    return intrinsic(type, spot, strike);
  }

  const double euro = bsPrice(type, spot, strike, t, rate, carry, vol);

  if (type == OptionType::CALL && carry >= rate)
  {
    return euro;  // early exercise never optimal
  }

  const double v2 = vol * vol;
  const double nn = 2.0 * carry / v2;
  const double m = 2.0 * rate / v2;
  const double K = 1.0 - std::exp(-rate * t);
  const double carryDisc = std::exp((carry - rate) * t);

  const double sk = detail::bawCriticalPrice(type, strike, t, rate, carry, vol);
  const double d1k = bsD1(sk, strike, t, carry, vol);

  if (type == OptionType::CALL)
  {
    if (spot >= sk)
    {
      return spot - strike;  // immediate exercise region
    }
    const double q2 = (-(nn - 1.0) + std::sqrt((nn - 1.0) * (nn - 1.0) + 4.0 * m / K)) / 2.0;
    const double a2 = (sk / q2) * (1.0 - carryDisc * normCdf(d1k));
    return euro + a2 * std::pow(spot / sk, q2);
  }

  if (spot <= sk)
  {
    return strike - spot;  // immediate exercise region
  }
  const double q1 = (-(nn - 1.0) - std::sqrt((nn - 1.0) * (nn - 1.0) + 4.0 * m / K)) / 2.0;
  const double a1 = -(sk / q1) * (1.0 - carryDisc * normCdf(-d1k));
  return euro + a1 * std::pow(spot / sk, q1);
}

// Engine dispatch by exercise style: European routes to closed-form
// Black-Scholes, American to Barone-Adesi-Whaley. Transparent to the caller —
// pass the SymbolInfo's exerciseStyle and get the right model.
inline double optionPrice(ExerciseStyle style, OptionType type, double spot, double strike,
                          double t, double rate, double carry, double vol) noexcept
{
  return style == ExerciseStyle::American ? bawPrice(type, spot, strike, t, rate, carry, vol)
                                          : bsPrice(type, spot, strike, t, rate, carry, vol);
}

}  // namespace flox::pricing
