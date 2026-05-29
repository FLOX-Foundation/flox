#pragma once

#include "flox/common.h"

#include <cmath>
#include <limits>
#include <numbers>

// Generalized Black-Scholes-Merton (cost-of-carry form). One model covers
// every vanilla European case by varying the carry rate b:
//   b = r           plain Black-Scholes (non-dividend stock)
//   b = r - q       continuous dividend yield q
//   b = 0           Black-76 option on a future / forward
//   b = r - rf      Garman-Kohlhagen FX option
// Crypto (Deribit) options are European and typically priced off the forward,
// so r = b = 0 is the common crypto case.

namespace flox::pricing
{

// Standard normal CDF via erfc (no external dependency).
inline double normCdf(double x) noexcept
{
  return 0.5 * std::erfc(-x / std::numbers::sqrt2);
}

// Standard normal PDF.
inline double normPdf(double x) noexcept
{
  static const double inv_sqrt_2pi = 1.0 / std::sqrt(2.0 * std::numbers::pi);
  return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

// Intrinsic value (undiscounted) for an expired or zero-vol option.
inline double intrinsic(OptionType type, double spot, double strike) noexcept
{
  return type == OptionType::CALL ? std::max(spot - strike, 0.0)
                                  : std::max(strike - spot, 0.0);
}

// d1 of the generalized BSM. Caller must guarantee vol > 0 and t > 0.
inline double bsD1(double spot, double strike, double t, double carry, double vol) noexcept
{
  return (std::log(spot / strike) + (carry + 0.5 * vol * vol) * t) / (vol * std::sqrt(t));
}

// Generalized Black-Scholes-Merton price.
//   spot   underlying price S
//   strike strike K
//   t      time to expiry in years
//   rate   risk-free discount rate r
//   carry  cost-of-carry b (see header note)
//   vol    annualized volatility sigma
// Degenerate inputs (t<=0 or vol<=0) collapse to discounted intrinsic so the
// function never returns NaN on a well-formed contract.
inline double bsPrice(OptionType type, double spot, double strike, double t, double rate,
                      double carry, double vol) noexcept
{
  if (spot <= 0.0 || strike <= 0.0)
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (t <= 0.0 || vol <= 0.0)
  {
    // Forward intrinsic discounted to present value.
    const double fwd = spot * std::exp(carry * t);
    const double disc = std::exp(-rate * t);
    return disc * intrinsic(type, fwd, strike);
  }

  const double d1 = bsD1(spot, strike, t, carry, vol);
  const double d2 = d1 - vol * std::sqrt(t);
  const double carryDisc = std::exp((carry - rate) * t);
  const double rateDisc = std::exp(-rate * t);

  if (type == OptionType::CALL)
  {
    return spot * carryDisc * normCdf(d1) - strike * rateDisc * normCdf(d2);
  }
  return strike * rateDisc * normCdf(-d2) - spot * carryDisc * normCdf(-d1);
}

// Vega (per 1.0 = 100 vol points) of the generalized BSM. Shared with the IV
// solver and the greeks module (W16.T003). Returns 0 for degenerate inputs.
inline double bsVega(double spot, double strike, double t, double rate, double carry,
                     double vol) noexcept
{
  if (spot <= 0.0 || strike <= 0.0 || t <= 0.0 || vol <= 0.0)
  {
    return 0.0;
  }
  const double d1 = bsD1(spot, strike, t, carry, vol);
  const double carryDisc = std::exp((carry - rate) * t);
  return spot * carryDisc * normPdf(d1) * std::sqrt(t);
}

struct ImpliedVolResult
{
  double vol = std::numeric_limits<double>::quiet_NaN();
  bool converged = false;
  int iterations = 0;
};

// Implied volatility from an observed option price. Newton-Raphson seeded by a
// Brenner-Subrahmanyam ATM guess, with bracketed bisection fallback when vega
// collapses (deep ITM/OTM) or Newton steps leave the [lo, hi] bracket. Returns
// NaN if the price violates no-arbitrage bounds (below intrinsic / above spot).
inline ImpliedVolResult impliedVol(OptionType type, double price, double spot, double strike,
                                   double t, double rate, double carry, double volLo = 1e-6,
                                   double volHi = 10.0, double tol = 1e-8,
                                   int maxIter = 100) noexcept
{
  ImpliedVolResult res;
  if (spot <= 0.0 || strike <= 0.0 || t <= 0.0 || price <= 0.0)
  {
    return res;
  }

  // No-arbitrage bounds: price must sit within [discounted intrinsic, asset bound].
  const double carryDisc = std::exp((carry - rate) * t);
  const double rateDisc = std::exp(-rate * t);
  const double loBound = bsPrice(type, spot, strike, t, rate, carry, volLo);
  const double hiBound = bsPrice(type, spot, strike, t, rate, carry, volHi);
  if (price < loBound - 1e-12 || price > hiBound + 1e-12)
  {
    return res;  // outside representable vol range → no solution
  }

  // Brenner-Subrahmanyam ATM seed: sigma ~ price/S * sqrt(2pi/T).
  double vol = (price / spot) * std::sqrt(2.0 * std::numbers::pi / t);
  if (!(vol > volLo) || !(vol < volHi))
  {
    vol = 0.5 * (volLo + volHi);
  }

  // Convergence is measured in vol space, not price space: for deep ITM/OTM
  // options the price is near-flat in vol (tiny vega), so a loose price
  // tolerance would "converge" at the wrong vol. Bracket bisection guarantees
  // we narrow the actual root to volTol regardless of vega.
  double lo = volLo;
  double hi = volHi;
  for (int i = 0; i < maxIter; ++i)
  {
    res.iterations = i + 1;
    const double model = bsPrice(type, spot, strike, t, rate, carry, vol);
    const double diff = model - price;
    // Maintain the bracket: price is monotone increasing in vol.
    if (diff > 0.0)
    {
      hi = vol;
    }
    else
    {
      lo = vol;
    }

    const double vega = bsVega(spot, strike, t, rate, carry, vol);
    double next;
    if (vega > 1e-10)
    {
      next = vol - diff / vega;  // Newton step
    }
    else
    {
      next = 0.5 * (lo + hi);  // vega collapsed → bisect
    }
    // Reject steps that escape the bracket → bisect instead.
    if (!(next > lo) || !(next < hi))
    {
      next = 0.5 * (lo + hi);
    }
    if (std::fabs(next - vol) < tol)
    {
      res.vol = next;
      res.converged = true;
      return res;
    }
    vol = next;
  }

  res.vol = vol;
  res.converged = false;
  return res;
}

// Forward price under cost-of-carry b (handy for crypto where options are
// quoted off the forward / index).
inline double forwardPrice(double spot, double t, double carry) noexcept
{
  return spot * std::exp(carry * t);
}

}  // namespace flox::pricing
