#pragma once

#include "flox/common.h"
#include "flox/pricing/black_scholes.h"

#include <cmath>
#include <vector>

// Cost-of-carry generalization that unifies crypto and TradFi financing. The
// generalized Black-Scholes (black_scholes.h) already prices off a single carry
// rate b; this header builds b from the pieces each asset class actually has:
//   crypto perp : b = 0          (funding modeled separately) or b = -funding
//   equity      : b = r - q - borrow   (dividend yield q, stock borrow / repo)
//   future      : b = 0          (Black-76)
//   FX          : b = r_dom - r_for
// plus a discrete-cash-dividend European pricer (the escrowed-dividend model),
// since real single-name equities pay lumpy dividends, not a smooth yield.

namespace flox::pricing
{

// A discrete cash dividend: `amount` per share paid `tYears` from valuation.
struct Dividend
{
  double tYears{0.0};
  double amount{0.0};
};

// Cost-of-carry b. dividendYield is the continuous yield q (or the foreign rate
// for FX); borrowRate is the stock borrow / repo cost. Crypto passes all zero.
inline double costOfCarry(double rate, double dividendYield, double borrowRate = 0.0) noexcept
{
  return rate - dividendYield - borrowRate;
}

// Present value of the cash dividends paid strictly before tExpiry, discounted
// at the risk-free rate. Dividends on or after expiry don't affect the option.
inline double presentValueOfDividends(const std::vector<Dividend>& divs, double rate,
                                      double tExpiry) noexcept
{
  double pv = 0.0;
  for (const auto& d : divs)
  {
    if (d.tYears > 0.0 && d.tYears < tExpiry)
    {
      pv += d.amount * std::exp(-rate * d.tYears);
    }
  }
  return pv;
}

// European price on a stock paying discrete cash dividends (escrowed-dividend
// model): the holder of the stock — not the option — receives the dividends, so
// subtract their present value from the spot and price the remainder as a
// non-dividend stock (carry = rate). A dividend lowers a call and lifts a put.
inline double bsPriceDiscreteDividends(OptionType type, double spot, double strike, double t,
                                       double rate, double vol, const std::vector<Dividend>& divs)
{
  const double adjustedSpot = spot - presentValueOfDividends(divs, rate, t);
  return bsPrice(type, adjustedSpot, strike, t, rate, /*carry=*/rate, vol);
}

}  // namespace flox::pricing
