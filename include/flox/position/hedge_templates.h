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
#include "flox/position/delta_hedger.h"
#include "flox/pricing/black_scholes.h"
#include "flox/pricing/greeks.h"

#include <cstdint>
#include <vector>

// Ready-made hedge structures, backtestable end-to-end over a price path. Two
// templates ship here:
//   - gammaScalp: long an option (long gamma, short theta), delta-hedged with a
//     perp at a rebalance band. Profits when realized vol exceeds the implied
//     vol the option was bought at — the rehedge trades buy low / sell high
//     around the delta, harvesting realized variance; theta (in the option's
//     mark decay) and transaction cost are what it pays for that.
//   - protectivePut: hold the underlying and buy a put as insurance. The put
//     caps downside; on a crash its gain offsets the underlying's loss.
//
// Each step marks the option with Black-Scholes (pricing/black_scholes.h) and
// its delta with the greeks module (pricing/greeks.h), so the backtest is
// self-contained and needs no live option feed. PnL is attributed into option /
// hedge / cost so the source of every dollar is explicit.

namespace flox
{

// One step of the backtest path. The option is marked at this spot, time to
// expiry, and implied vol — vol can vary per step to model a moving surface.
struct HedgePathPoint
{
  double spot{0.0};  // underlying / index price
  double t{0.0};     // time to expiry in years (decreases toward 0)
  double vol{0.0};   // implied vol used to mark the option at this step
};

// PnL attribution for a hedge backtest. Every term is in cash. optionPnl is the
// option leg's mark-to-market change (which already embeds its theta decay, so
// theta is NOT subtracted again here); hedgePnl is the underlying / perp leg;
// transactionCost and fundingCost are the running cash cost of the hedge.
struct HedgeBacktestResult
{
  double optionPnl{0.0};
  double hedgePnl{0.0};
  double transactionCost{0.0};
  double fundingCost{0.0};
  uint64_t rebalances{0};

  double net() const { return optionPnl + hedgePnl - transactionCost - fundingCost; }
};

// Gamma scalp: hold optionQty of one option and keep the book delta-neutral by
// trading a perp at the band. costBps is fees + slippage per rehedge trade;
// fundingRatePerStep is the perp funding rate applied to the hedge notional each
// step (0 to ignore funding). The path must have >= 2 points.
inline HedgeBacktestResult gammaScalp(OptionType type, double strike, double rate, double carry,
                                      double optionQty, double band, double costBps,
                                      double fundingRatePerStep,
                                      const std::vector<HedgePathPoint>& path)
{
  HedgeBacktestResult r;
  if (path.size() < 2)
  {
    return r;
  }

  DeltaHedger hedger(band);

  // Seed the hedge at the first mark so the book starts delta-neutral.
  {
    const auto g = pricing::greeks(type, path[0].spot, strike, path[0].t, rate, carry, path[0].vol);
    const auto adj = hedger.step(optionQty * g.delta);
    if (adj.rebalanced)
    {
      r.transactionCost += std::fabs(adj.quantity) * path[0].spot * costBps * 1e-4;
      ++r.rebalances;
    }
  }

  for (size_t i = 1; i < path.size(); ++i)
  {
    const auto& prev = path[i - 1];
    const auto& cur = path[i];

    // Option leg marked to market across the step.
    const double markPrev = pricing::bsPrice(type, prev.spot, strike, prev.t, rate, carry, prev.vol);
    const double markCur = pricing::bsPrice(type, cur.spot, strike, cur.t, rate, carry, cur.vol);
    r.optionPnl += optionQty * (markCur - markPrev);

    // Perp hedge leg earns the price move on the position carried into the step.
    const double hedgePos = hedger.hedgePosition();
    r.hedgePnl += hedgePos * (cur.spot - prev.spot);

    // Funding paid to carry the perp hedge notional over the step.
    r.fundingCost += std::fabs(hedgePos) * cur.spot * fundingRatePerStep;

    // Rehedge on the new option delta if net delta left the band.
    const auto g = pricing::greeks(type, cur.spot, strike, cur.t, rate, carry, cur.vol);
    const double netDelta = optionQty * g.delta + hedger.hedgePosition();
    const auto adj = hedger.step(netDelta);
    if (adj.rebalanced)
    {
      r.transactionCost += std::fabs(adj.quantity) * cur.spot * costBps * 1e-4;
      ++r.rebalances;
    }
  }
  return r;
}

// Protective put: hold underlyingQty of the underlying and putQty long puts as
// insurance. A static structure — no rehedging, so no transaction or funding
// cost. hedgePnl is the underlying leg; optionPnl is the put leg. The path must
// have >= 2 points.
inline HedgeBacktestResult protectivePut(double strike, double rate, double carry,
                                         double underlyingQty, double putQty,
                                         const std::vector<HedgePathPoint>& path)
{
  HedgeBacktestResult r;
  if (path.size() < 2)
  {
    return r;
  }

  for (size_t i = 1; i < path.size(); ++i)
  {
    const auto& prev = path[i - 1];
    const auto& cur = path[i];

    const double putPrev =
        pricing::bsPrice(OptionType::PUT, prev.spot, strike, prev.t, rate, carry, prev.vol);
    const double putCur =
        pricing::bsPrice(OptionType::PUT, cur.spot, strike, cur.t, rate, carry, cur.vol);
    r.optionPnl += putQty * (putCur - putPrev);
    r.hedgePnl += underlyingQty * (cur.spot - prev.spot);
  }
  return r;
}

}  // namespace flox
