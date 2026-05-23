/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/liquidation_engine.h"
#include "flox/common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace flox
{

// Margin mode for an Account. Real venues let traders pick per
// position; for backtest purposes we treat it as an account-level
// switch.
//   Cross    — equity is shared across all positions on the account.
//              A profitable BTC short backs a losing ETH long. MM
//              checks evaluate `equity + sum_upnl` vs
//              `sum_notional × mm_tier(sum_notional)`.
//   Isolated — equity is posted per position (the existing
//              LeveragedPosition.equity field carries it). Each
//              position liquidates independently.
enum class MarginMode : uint8_t
{
  Cross = 0,
  Isolated = 1,
};

// Account-level state shared across W15 subsystems. Owns positions,
// shared equity, per-symbol marks, and a 30-day rolling notional
// counter that FeeSchedule can consume in place of its own internal
// counter via bindAccount.
//
// The class is non-owning from the engine's perspective: callers
// build an Account, then attach it to LiquidationEngine and/or
// FeeSchedule. Lifetime is the caller's responsibility.
class Account
{
 public:
  static constexpr int64_t kThirtyDaysNs = 30LL * 24LL * 3600LL * 1'000'000'000LL;

  Account() = default;
  Account(uint64_t accountId, double equity)
      : _accountId(accountId), _equity(equity)
  {
  }

  uint64_t accountId() const noexcept { return _accountId; }

  double equity() const noexcept { return _equity; }
  void setEquity(double e) noexcept { _equity = e; }
  void addEquity(double delta) noexcept { _equity += delta; }

  MarginMode marginMode() const noexcept { return _mode; }
  void setMarginMode(MarginMode mode) noexcept { _mode = mode; }
  // Accepts "cross" / "isolated" (case-insensitive). Unknown names
  // are ignored.
  void setMarginModeByName(const std::string& name);

  // Open / close positions. Side encoded in signed `quantity`.
  void openPosition(SymbolId symbol, double quantity, double entryPrice);
  void closePosition(SymbolId symbol);
  const std::vector<LeveragedPosition>& positions() const noexcept { return _positions; }
  std::vector<LeveragedPosition>& positionsMut() noexcept { return _positions; }
  size_t positionCount() const noexcept { return _positions.size(); }

  // Per-symbol mark prices. Used for cross-margin MM evaluation; the
  // attached LiquidationEngine updates the mark for the current
  // symbol before walking the account. Positions on symbols without
  // a mark are valued at entry price (zero uPnL).
  void setMark(SymbolId symbol, double price);
  double markFor(SymbolId symbol) const;

  // 30-day rolling notional counter. recordFill pushes a fill into
  // the window; rollingNotional30d returns the current sum. Used by
  // FeeSchedule when bound.
  void recordFill(int64_t tsNs, double notional);
  double rollingNotional30d() const noexcept { return _rollingTotal; }
  void resetRolling() noexcept
  {
    _rolling.clear();
    _rollingTotal = 0.0;
  }

  // Aggregate views over the position book.
  double totalNotional() const;
  double totalUnrealisedPnl() const;
  // Account-level cross-margin equity headroom: equity + total uPnL
  // minus the maintenance margin required at the total-notional tier.
  // Negative = account is underwater and should be liquidated.
  double crossHeadroom(double tierFraction) const;

 private:
  void evictExpired(int64_t nowNs);

  uint64_t _accountId{0};
  double _equity{0.0};
  MarginMode _mode{MarginMode::Cross};
  std::vector<LeveragedPosition> _positions;
  std::vector<std::pair<SymbolId, double>> _marks;
  std::deque<std::pair<int64_t, double>> _rolling;
  double _rollingTotal{0.0};
};

}  // namespace flox
