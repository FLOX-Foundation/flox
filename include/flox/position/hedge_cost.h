/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/position/delta_hedger.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// Cost accounting for a delta hedge, and a rebalance-band optimizer.
//
// The total cost of running a delta hedge splits into three parts:
//   - theta cost: the option book decays over time (a long book's negative theta
//     is a running cost).
//   - funding cost: funding paid to carry the perp hedge leg.
//   - transaction cost: fees + slippage on each rehedge trade.
// The band trades transaction cost against tracking error: a tight band rehedges
// often (more transaction cost, tighter delta) while a wide band carries residual
// delta between rehedges (less cost, more PnL drift).

namespace flox
{

struct CostOfHedge
{
  double thetaCost{0.0};
  double fundingCost{0.0};
  double transactionCost{0.0};

  double total() const { return thetaCost + fundingCost + transactionCost; }
};

class HedgeCostAccumulator
{
 public:
  // Option-book decay over dtYears. thetaPerYear is the portfolio theta (negative
  // for a long book), so the running cost is -theta * dt.
  void accrueTheta(double thetaPerYear, double dtYears) { _c.thetaCost += -thetaPerYear * dtYears; }

  // Funding paid on the perp hedge leg over a step (already in cash terms).
  void accrueFunding(double fundingPaid) { _c.fundingCost += fundingPaid; }

  // Fees + slippage on a rehedge trade: |units| * price * costBps / 1e4.
  void accrueTransaction(double tradeUnits, double price, double costBps)
  {
    _c.transactionCost += std::fabs(tradeUnits) * price * costBps * 1e-4;
  }

  const CostOfHedge& cost() const { return _c; }

 private:
  CostOfHedge _c;
};

// One step of the option book's delta path, with the mark used to value trades
// and price moves.
struct HedgePathStep
{
  double optionDelta{0.0};  // the option book's delta at this step (pre-hedge)
  double mark{0.0};         // underlying / hedge price at this step
};

struct BandResult
{
  double band{0.0};
  double transactionCost{0.0};
  double trackingError{0.0};  // sum of |residual delta| * |price move|
  uint64_t rebalances{0};

  double total() const { return transactionCost + trackingError; }
};

// Simulate the band hedger over an option-delta path. At each step the perp hedge
// contributes its position's delta; if net delta leaves the band the hedger
// trades (transaction cost). The residual net delta carried to the next step
// times the price move is the tracking error. costBps is fees + slippage per
// trade.
inline BandResult evaluateBand(double band, const std::vector<HedgePathStep>& path, double costBps)
{
  BandResult r;
  r.band = band;
  DeltaHedger h(band);

  for (size_t i = 0; i < path.size(); ++i)
  {
    const double netDelta = path[i].optionDelta + h.hedgePosition();
    const auto adj = h.step(netDelta);
    if (adj.rebalanced)
    {
      r.transactionCost += std::fabs(adj.quantity) * path[i].mark * costBps * 1e-4;
      ++r.rebalances;
    }
    const double residual = path[i].optionDelta + h.hedgePosition();
    if (i + 1 < path.size())
    {
      r.trackingError += std::fabs(residual) * std::fabs(path[i + 1].mark - path[i].mark);
    }
  }
  return r;
}

// Sweep candidate bands and return the result per band; the caller picks the
// minimum total. `bestBand` is provided as a convenience.
inline std::vector<BandResult> sweepBands(const std::vector<double>& bands,
                                          const std::vector<HedgePathStep>& path, double costBps)
{
  std::vector<BandResult> out;
  out.reserve(bands.size());
  for (double b : bands)
  {
    out.push_back(evaluateBand(b, path, costBps));
  }
  return out;
}

inline double bestBand(const std::vector<BandResult>& results)
{
  double best = std::numeric_limits<double>::quiet_NaN();
  double bestTotal = std::numeric_limits<double>::infinity();
  for (const auto& r : results)
  {
    if (r.total() < bestTotal)
    {
      bestTotal = r.total();
      best = r.band;
    }
  }
  return best;
}

}  // namespace flox
