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

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace flox
{

// One maintenance-margin tier. `minNotional` is the lower bound of
// the bracket (inclusive); `mmFraction` is the maintenance-margin
// rate at that bracket (e.g. 0.005 = 0.5%).
struct MarginTier
{
  double minNotional{0.0};
  double mmFraction{0.0};
};

// A single open position the LiquidationEngine watches. Side +ve =
// long, -ve = short (signed quantity). Equity is the residual
// account balance backing this position; if liquidation slippage
// burns through it, the deficit hits the insurance fund.
struct LeveragedPosition
{
  uint64_t accountId{0};
  SymbolId symbol{};
  double quantity{0.0};  // signed: + long, - short
  double entryPrice{0.0};
  double equity{0.0};  // margin posted backing this position
};

// Outcome of the engine's check at a given (mark, tick) snapshot.
struct LiquidationOutcome
{
  std::vector<uint64_t> liquidated;    // accounts the engine force-closed
  std::vector<uint64_t> adlClosedOut;  // accounts ADL hit on the opposite side
  double insuranceFundDelta{0.0};      // signed: + payments in, - payments out
  uint64_t liquidationsCount{0};
  uint64_t insurancePaymentsCount{0};
  uint64_t adlCloseoutsCount{0};
};

// Liquidation + insurance-fund + auto-deleverage engine. Holds the
// venue-wide state (margin tiers, fund balance, ADL toggle) and
// the live position book. `onMark` walks every position against
// the new mark price, fires liquidations / ADL where required, and
// returns the resulting actions plus running stats.
//
// The engine deliberately does NOT route through SimulatedExecutor
// directly: integrating with the order queue requires per-symbol
// L2 book state which most backtests don't model. For research-
// grade cascade simulation, feed positions in / mark prices in,
// observe what gets liquidated, and account for the slippage cost
// returned in `slippageBps`.
class LiquidationEngine
{
 public:
  LiquidationEngine() = default;

  // Tier configuration. Tiers are sorted ascending by minNotional
  // on each addTier call.
  LiquidationEngine& addTier(double minNotional, double mmFraction)
  {
    _tiers.push_back({minNotional, mmFraction});
    std::sort(_tiers.begin(), _tiers.end(),
              [](const MarginTier& a, const MarginTier& b)
              { return a.minNotional < b.minNotional; });
    return *this;
  }
  const std::vector<MarginTier>& tiers() const { return _tiers; }

  // Insurance fund balance. Force-close losses exceeding the
  // trader's equity are paid from this balance until depleted.
  void setInsuranceFundCapital(double capital) noexcept
  {
    _insuranceFund = capital;
  }
  double insuranceFundBalance() const noexcept { return _insuranceFund; }

  // ADL toggle. When true and the insurance fund cannot cover a
  // liquidation deficit, the engine force-closes profitable
  // opposite-side positions ranked by PnL until the deficit is
  // absorbed.
  void setAdlEnabled(bool enabled) noexcept { _adlEnabled = enabled; }
  bool adlEnabled() const noexcept { return _adlEnabled; }

  // Liquidation slippage in basis points applied to the bankruptcy
  // close. Default 25 bps. Use higher values for venues with thin
  // books or volatile derivatives.
  void setLiquidationSlippageBps(double bps) noexcept
  {
    _slippageBps = bps;
  }
  double liquidationSlippageBps() const noexcept { return _slippageBps; }

  // Open a new position the engine should track.
  void openPosition(const LeveragedPosition& position)
  {
    _positions.push_back(position);
  }
  // Forcefully forget a position (caller closed it cleanly).
  void closePosition(uint64_t accountId, SymbolId symbol)
  {
    _positions.erase(
        std::remove_if(_positions.begin(), _positions.end(),
                       [&](const LeveragedPosition& p)
                       { return p.accountId == accountId && p.symbol == symbol; }),
        _positions.end());
  }
  const std::vector<LeveragedPosition>& positions() const { return _positions; }

  // Walk every position against `markPrice` for `symbol`. Returns
  // the actions taken on this tick. Positions that fall below
  // maintenance margin are liquidated at
  // `markPrice * (1 - sign(qty) * slippageBps/10000)`; deficits hit
  // the insurance fund, then ADL.
  LiquidationOutcome onMark(SymbolId symbol, double markPrice);

  // Cumulative stats across all onMark calls since construction.
  uint64_t liquidationsCount() const noexcept { return _statLiquidations; }
  uint64_t insurancePaymentsCount() const noexcept { return _statInsurancePayments; }
  uint64_t adlCloseoutsCount() const noexcept { return _statAdlCloseouts; }

  // Resolve the maintenance-margin fraction for a given notional.
  // Walks the tier list and returns the rate for the highest tier
  // whose minNotional <= notional. Returns 0 if no tiers configured.
  double mmFractionFor(double notional) const;

  // Canned profiles: realistic tier ladders + insurance balances.
  static LiquidationEngine binance_um_futures();
  static LiquidationEngine bybit_linear();
  static LiquidationEngine okx_swap();

 private:
  std::vector<MarginTier> _tiers;
  std::vector<LeveragedPosition> _positions;
  double _insuranceFund{0.0};
  bool _adlEnabled{true};
  double _slippageBps{25.0};

  uint64_t _statLiquidations{0};
  uint64_t _statInsurancePayments{0};
  uint64_t _statAdlCloseouts{0};
};

}  // namespace flox
