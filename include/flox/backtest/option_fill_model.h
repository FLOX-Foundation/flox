/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"

#include <cstddef>
#include <vector>

// Realistic fills for illiquid options. Backtesting option strategies on the
// mid (or worse, the close) manufactures alpha that evaporates live: real fills
// cross part of a wide, sparse bid-ask spread. This models that honestly.
//
//   - A market order does not fill at the mid. It crosses a fraction of the
//     half-spread toward the touch (single-leg ~0.75 of the way; ORATS).
//   - Multi-leg spreads fill atomically with LESS slippage per leg — a package
//     gets price improvement, down to ~0.56 for a four-leg spread (ORATS). So
//     slippage is non-linear in the number of legs.
//   - A missing or locked/crossed quote does NOT produce a fake fill: the order
//     is reported unfilled, and the caller applies its no-quote policy.
//   - Market makers widen near the close, so a backtest can widen the quote to
//     model the last minutes rather than trusting the closing print.

namespace flox
{

struct OptionBBO
{
  double bid{0.0};
  double ask{0.0};

  // A usable two-sided quote: positive bid and an ask strictly above it. A
  // zero/absent side or a locked/crossed market (ask <= bid) is not usable.
  bool valid() const { return bid > 0.0 && ask > bid; }
  double mid() const { return 0.5 * (bid + ask); }
  double halfSpread() const { return 0.5 * (ask - bid); }
  double width() const { return ask - bid; }
};

// What a caller does when a leg has no usable quote. The fill functions never
// fabricate a fill; this records the caller's intent for the empty bar.
enum class NoQuoteAction
{
  Skip,    // skip the bar, try again next quote
  Hold,    // keep the position, do not trade
  Reject,  // reject the order outright
};

// Fraction of the half-spread an N-leg order crosses toward the touch. One leg
// crosses the most (~0.75); each added leg earns package price improvement, to
// ~0.56 at four legs, then flat. Linear between the ORATS endpoints.
inline double legCrossFraction(size_t numLegs)
{
  if (numLegs <= 1)
  {
    return 0.75;
  }
  if (numLegs >= 4)
  {
    return 0.56;
  }
  return 0.75 - (static_cast<double>(numLegs - 1)) * ((0.75 - 0.56) / 3.0);
}

struct FillResult
{
  bool filled{false};
  double price{0.0};     // realized fill price
  double slippage{0.0};  // cost vs mid, per unit (always >= 0 when filled)
};

// Single-leg market fill: cross legCrossFraction(1) of the half-spread toward
// the touch (above mid to buy, below mid to sell). An unusable quote returns
// unfilled — never a mid/close fake fill.
inline FillResult fillSingleLeg(Side side, const OptionBBO& q)
{
  FillResult r;
  if (!q.valid())
  {
    return r;
  }
  const double slip = legCrossFraction(1) * q.halfSpread();
  r.filled = true;
  r.slippage = slip;
  r.price = (side == Side::BUY) ? q.mid() + slip : q.mid() - slip;
  return r;
}

struct SpreadLeg
{
  Side side{Side::BUY};
  double qty{1.0};
  OptionBBO quote;
};

struct SpreadFillResult
{
  bool filled{false};
  double netDebit{0.0};  // signed package cost: + you pay, - you receive
  double slippage{0.0};  // total cost vs the net mid (always >= 0 when filled)
};

// Atomic multi-leg spread fill. All legs fill or none — if ANY leg lacks a
// usable quote the whole package is unfilled. The package crosses
// legCrossFraction(numLegs) of each leg's half-spread, so per-leg slippage falls
// as legs are added (non-linear). netDebit is the net mid plus that slippage.
inline SpreadFillResult fillSpread(const std::vector<SpreadLeg>& legs)
{
  SpreadFillResult r;
  if (legs.empty())
  {
    return r;
  }
  for (const auto& l : legs)
  {
    if (!l.quote.valid())
    {
      return r;  // atomic: one missing quote fails the whole spread
    }
  }
  const double cross = legCrossFraction(legs.size());
  double netMid = 0.0;
  double slip = 0.0;
  for (const auto& l : legs)
  {
    const double sign = (l.side == Side::BUY) ? 1.0 : -1.0;
    netMid += sign * l.qty * l.quote.mid();
    slip += l.qty * cross * l.quote.halfSpread();  // crossing always costs
  }
  r.filled = true;
  r.slippage = slip;
  r.netDebit = netMid + slip;
  return r;
}

// Widen a quote around its mid by factor (>= 1) to model the spreads market
// makers post near the close — a better proxy for the last minutes than the
// closing print itself.
inline OptionBBO widenQuote(const OptionBBO& q, double factor)
{
  const double m = q.mid();
  const double h = q.halfSpread() * factor;
  return OptionBBO{m - h, m + h};
}

}  // namespace flox
