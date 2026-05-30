/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cmath>
#include <cstdint>

// Band-based delta-neutral hedger. Given the portfolio's current net delta
// (from PortfolioGreeksAggregator, which already accounts for the live hedge
// leg), it emits the hedge trade that brings net delta back to zero — but only
// when the drift exceeds a band. The band trades tracking error against cost:
// a tight band hugs zero delta but rehedges constantly (fees + slippage), a
// wide band drifts further but trades less.

namespace flox
{

struct HedgeAdjustment
{
  double quantity{0.0};    // units to trade in the hedge leg; +buy, -sell
  bool rebalanced{false};  // true when a trade was emitted this step
};

class DeltaHedger
{
 public:
  // band: rehedge only when |net delta| > band (in delta units).
  // hedgeInstrumentDelta: delta per unit of the hedge leg — 1.0 for a linear
  // perp, or an option's delta if hedging with another option.
  explicit DeltaHedger(double band, double hedgeInstrumentDelta = 1.0)
      : _band(std::fabs(band)), _hedgeDelta(hedgeInstrumentDelta)
  {
  }

  // netDelta: the portfolio net delta INCLUDING the current hedge position. When
  // it drifts outside the band, returns the trade that neutralizes it and rolls
  // the internal hedge position forward; otherwise returns no trade.
  HedgeAdjustment step(double netDelta)
  {
    if (std::fabs(netDelta) <= _band || _hedgeDelta == 0.0)
    {
      return {0.0, false};
    }
    const double adj = -netDelta / _hedgeDelta;
    _hedgePos += adj;
    ++_rebalanceCount;
    _turnover += std::fabs(adj);
    return {adj, true};
  }

  double hedgePosition() const { return _hedgePos; }
  uint64_t rebalanceCount() const { return _rebalanceCount; }
  // Sum of |trade| across rehedges — the basis for fee / slippage cost.
  double turnover() const { return _turnover; }

  void reset()
  {
    _hedgePos = 0.0;
    _rebalanceCount = 0;
    _turnover = 0.0;
  }

 private:
  double _band;
  double _hedgeDelta;
  double _hedgePos{0.0};
  uint64_t _rebalanceCount{0};
  double _turnover{0.0};
};

}  // namespace flox
