/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace flox::risk
{

// One row in the aggregator's view. Mirrors flox_py.portfolio_risk
// .StrategyAccount; daily_pnl is derived (realized + unrealized + fees).
struct StrategyAccount
{
  std::string name;
  double realized_pnl{0.0};
  double unrealized_pnl{0.0};
  double fees{0.0};
  double gross_exposure{0.0};
  double net_exposure{0.0};
  uint64_t trade_count{0};

  double dailyPnl() const { return realized_pnl + unrealized_pnl + fees; }
};

// Portfolio-level limits. nullopt = "no cap on this dimension".
struct RiskRules
{
  std::optional<double> max_drawdown_pct{};
  std::optional<double> max_daily_loss{};
  std::optional<double> max_gross_exposure{};
  std::optional<double> max_concentration_pct{};
};

struct Breach
{
  std::string rule;
  double value{0.0};
  double limit{0.0};
  std::string detail;
};

struct PortfolioSnapshot
{
  double total_realized_pnl{0.0};
  double total_unrealized_pnl{0.0};
  double total_fees{0.0};
  double total_daily_pnl{0.0};
  double total_gross_exposure{0.0};
  double total_net_exposure{0.0};
  uint64_t total_trade_count{0};
  double current_equity{0.0};
  double peak_equity{0.0};
  double drawdown_pct{0.0};
  bool kill_switch_active{false};
  std::vector<Breach> active_breaches;
  std::vector<StrategyAccount> accounts;
};

class PortfolioRiskAggregator
{
 public:
  // A `max_drawdown_pct` rule needs a capital base to measure against. Under
  // the default `initial_equity` of zero the rule did nothing at all: a
  // 500,000 loss reported a drawdown of 0.0000, the kill switch stayed down
  // and every order went through. Once any profit landed, the peak became
  // that profit alone and a pullback from +1000 to +500 read as a 50%
  // drawdown on an account deep in the red. Asking for a drawdown cap without
  // a positive initial equity now throws std::invalid_argument.
  explicit PortfolioRiskAggregator(RiskRules rules = {},
                                   double initial_equity = 0.0);

  // Upsert one strategy's view. The mask controls which fields
  // of `update` are written; bits 0..5 correspond to
  // realized_pnl, unrealized_pnl, fees, gross_exposure,
  // net_exposure, trade_count. Missing strategy creates a new row.
  void update(const std::string& name, const StrategyAccount& fields,
              uint8_t field_mask);

  void remove(const std::string& name);
  void resetKillSwitch();

  // Pre-trade gate: returns the first matching breach, or nullopt
  // if the order is allowed. Does not mutate state.
  //
  // `side` is "buy" / "sell" / "reduce" (case-insensitive for the first two).
  // "reduce" always marks the order as risk-reducing; "buy" and "sell" are
  // read against the strategy's reported `net_exposure`, falling back to its
  // gross exposure when the caller has never reported a net. Risk-reducing
  // orders pass every state limit, which is the point of having them.
  std::optional<Breach> checkOrder(const std::string& strategy,
                                   double notional,
                                   const std::string& side) const;

  PortfolioSnapshot snapshot() const;

 private:
  // One stored row: the public account fields plus whether the caller has
  // ever reported a net exposure for this strategy. Without that flag a
  // caller who only feeds gross exposure -- the shape every example and every
  // test uses -- reads back a net of zero, and the "does this order reduce
  // risk" test answers "no" for every order ever submitted.
  struct AccountRow : StrategyAccount
  {
    bool net_exposure_reported{false};
  };

  // All called with _mutex held.
  double totalGrossLocked() const;
  double totalDailyPnlLocked() const;
  double currentEquityLocked() const;
  double drawdownPctLocked() const;
  size_t contributingAccountsLocked() const;
  std::vector<Breach> breachesLocked() const;
  void reevaluateLocked();
  PortfolioSnapshot buildSnapshotLocked() const;

  RiskRules _rules;
  double _initial_equity;
  double _peak_equity;
  std::map<std::string, AccountRow> _accounts;
  bool _kill_switch_active{false};
  mutable std::mutex _mutex;
};

namespace field_mask
{
inline constexpr uint8_t REALIZED_PNL = 1u << 0;
inline constexpr uint8_t UNREALIZED_PNL = 1u << 1;
inline constexpr uint8_t FEES = 1u << 2;
inline constexpr uint8_t GROSS_EXPOSURE = 1u << 3;
inline constexpr uint8_t NET_EXPOSURE = 1u << 4;
inline constexpr uint8_t TRADE_COUNT = 1u << 5;
inline constexpr uint8_t ALL = 0x3Fu;
}  // namespace field_mask

}  // namespace flox::risk
