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

  // Open a position. Side encoded in signed `quantity`. The
  // `isolatedEquity` slice is the margin posted backing this
  // position when the account runs in MarginMode::Isolated; it is
  // ignored in Cross mode (shared account equity is used instead).
  // Default 0.0 keeps backwards-compatible call sites; isolated-
  // mode callers MUST pass the slice or the position survives any
  // mark move (no equity backing → trivially solvent).
  void openPosition(SymbolId symbol, double quantity, double entryPrice,
                    double isolatedEquity = 0.0);
  void closePosition(SymbolId symbol);
  const std::vector<LeveragedPosition>& positions() const noexcept { return _positions; }
  std::vector<LeveragedPosition>& positionsMut() noexcept { return _positions; }
  size_t positionCount() const noexcept { return _positions.size(); }

  // Per-symbol mark prices. Used for cross-margin MM evaluation; the
  // attached LiquidationEngine updates the mark for the current
  // symbol before walking the account. Positions on symbols without
  // a mark are valued at entry price (zero uPnL).
  //
  // `tsNs` records the timestamp of the mark update; callers using
  // the stale-mark guard (T053) must pass a real timestamp. The
  // default 0 keeps backwards compatibility with callers that don't
  // care about staleness checks.
  void setMark(SymbolId symbol, double price, int64_t tsNs = 0);
  double markFor(SymbolId symbol) const;
  // Last timestamp any setMark was called for `symbol`. Returns
  // INT64_MIN when the symbol has never been marked. Used by the
  // stale-mark guard.
  int64_t markTsFor(SymbolId symbol) const;

  // Stale-mark guard. Returns true when any position in the
  // account is for a symbol whose last mark is older than
  // `budgetNs` relative to `nowNs`, or when the symbol has never
  // been marked. Use this BEFORE invoking onMark / onMarks if the
  // backtest must refuse to walk under stale data.
  bool hasStaleMarks(int64_t nowNs, int64_t budgetNs) const;

  // 30-day rolling notional counter. recordFill pushes a fill into
  // the window; rollingNotional30d returns the current sum. Used by
  // FeeSchedule when bound. `symbol` (default 0, "unknown") lets the
  // caller break down rolling notional by symbol for venue tier
  // overrides or analytics; FeeSchedule still reads the aggregate.
  void recordFill(int64_t tsNs, double notional, SymbolId symbol = 0);
  double rollingNotional30d() const noexcept { return _rollingTotal; }
  // Per-symbol rolling notional within the current 30d window.
  // Symbols never seen by recordFill (or whose fills have all been
  // evicted) are absent from the result. The fallback `symbol = 0`
  // bucket carries fills recorded without an explicit symbol.
  std::vector<std::pair<SymbolId, double>> rollingNotionalBySymbol30d() const;
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
  // Marks are stored as (symbol, price, last-update ts). ts is
  // INT64_MIN when unset; callers that don't pass a ts to setMark
  // keep ts at 0 and the stale-mark guard treats them as fresh.
  struct Mark
  {
    SymbolId symbol;
    double price;
    int64_t tsNs;
  };
  std::vector<Mark> _marks;
  struct RollingFill
  {
    int64_t tsNs;
    double notional;
    SymbolId symbol;
  };
  std::deque<RollingFill> _rolling;
  double _rollingTotal{0.0};
};

}  // namespace flox
