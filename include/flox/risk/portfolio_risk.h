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
  std::optional<Breach> checkOrder(const std::string& strategy,
                                   double notional,
                                   const std::string& side) const;

  PortfolioSnapshot snapshot() const;

 private:
  // All called with _mutex held.
  double totalGrossLocked() const;
  double totalDailyPnlLocked() const;
  double currentEquityLocked() const;
  double drawdownPctLocked() const;
  std::vector<Breach> breachesLocked() const;
  void reevaluateLocked();
  PortfolioSnapshot buildSnapshotLocked() const;

  RiskRules _rules;
  double _initial_equity;
  double _peak_equity;
  std::map<std::string, StrategyAccount> _accounts;
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
