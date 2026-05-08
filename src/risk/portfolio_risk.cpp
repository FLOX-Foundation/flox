/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/risk/portfolio_risk.h"

#include <algorithm>
#include <cmath>

namespace flox::risk
{

PortfolioRiskAggregator::PortfolioRiskAggregator(RiskRules rules,
                                                 double initial_equity)
    : _rules(std::move(rules)),
      _initial_equity(initial_equity),
      _peak_equity(initial_equity)
{
}

void PortfolioRiskAggregator::update(const std::string& name,
                                     const StrategyAccount& fields,
                                     uint8_t field_mask)
{
  std::lock_guard<std::mutex> guard(_mutex);
  auto& row = _accounts[name];
  row.name = name;
  if (field_mask & field_mask::REALIZED_PNL)
  {
    row.realized_pnl = fields.realized_pnl;
  }
  if (field_mask & field_mask::UNREALIZED_PNL)
  {
    row.unrealized_pnl = fields.unrealized_pnl;
  }
  if (field_mask & field_mask::FEES)
  {
    row.fees = fields.fees;
  }
  if (field_mask & field_mask::GROSS_EXPOSURE)
  {
    row.gross_exposure = fields.gross_exposure;
  }
  if (field_mask & field_mask::NET_EXPOSURE)
  {
    row.net_exposure = fields.net_exposure;
  }
  if (field_mask & field_mask::TRADE_COUNT)
  {
    row.trade_count = fields.trade_count;
  }
  reevaluateLocked();
}

void PortfolioRiskAggregator::remove(const std::string& name)
{
  std::lock_guard<std::mutex> guard(_mutex);
  _accounts.erase(name);
  reevaluateLocked();
}

void PortfolioRiskAggregator::resetKillSwitch()
{
  std::lock_guard<std::mutex> guard(_mutex);
  _kill_switch_active = false;
}

std::optional<Breach> PortfolioRiskAggregator::checkOrder(const std::string& /*strategy*/,
                                                          double notional,
                                                          const std::string& /*side*/) const
{
  std::lock_guard<std::mutex> guard(_mutex);
  if (_kill_switch_active)
  {
    return Breach{"kill_switch_active", 1.0, 0.0,
                  "portfolio kill switch is engaged"};
  }
  if (_rules.max_gross_exposure.has_value())
  {
    const double cap = *_rules.max_gross_exposure;
    const double proposed = totalGrossLocked() + std::abs(notional);
    if (proposed > cap)
    {
      return Breach{"max_gross_exposure", proposed, cap,
                    "order would push gross exposure to " + std::to_string(proposed) + " (cap " + std::to_string(cap) + ")"};
    }
  }
  return std::nullopt;
}

PortfolioSnapshot PortfolioRiskAggregator::snapshot() const
{
  std::lock_guard<std::mutex> guard(_mutex);
  return buildSnapshotLocked();
}

double PortfolioRiskAggregator::totalGrossLocked() const
{
  double s = 0.0;
  for (const auto& [_, a] : _accounts)
  {
    s += a.gross_exposure;
  }
  return s;
}

double PortfolioRiskAggregator::totalDailyPnlLocked() const
{
  double s = 0.0;
  for (const auto& [_, a] : _accounts)
  {
    s += a.dailyPnl();
  }
  return s;
}

double PortfolioRiskAggregator::currentEquityLocked() const
{
  return _initial_equity + totalDailyPnlLocked();
}

double PortfolioRiskAggregator::drawdownPctLocked() const
{
  if (_peak_equity <= 0)
  {
    return 0.0;
  }
  const double cur = currentEquityLocked();
  return std::max(0.0, (_peak_equity - cur) / _peak_equity);
}

std::vector<Breach> PortfolioRiskAggregator::breachesLocked() const
{
  std::vector<Breach> out;
  if (_rules.max_drawdown_pct.has_value())
  {
    const double dd = drawdownPctLocked();
    if (dd > *_rules.max_drawdown_pct)
    {
      out.push_back({"max_drawdown_pct", dd, *_rules.max_drawdown_pct,
                     "drawdown " + std::to_string(dd) + " over cap " + std::to_string(*_rules.max_drawdown_pct)});
    }
  }
  if (_rules.max_daily_loss.has_value())
  {
    const double pnl = totalDailyPnlLocked();
    const double cap = std::abs(*_rules.max_daily_loss);
    if (pnl < 0.0 && std::abs(pnl) > cap)
    {
      out.push_back({"max_daily_loss", pnl, -cap,
                     "daily PnL " + std::to_string(pnl) + " below allowed loss " + std::to_string(-cap)});
    }
  }
  if (_rules.max_gross_exposure.has_value())
  {
    const double gross = totalGrossLocked();
    if (gross > *_rules.max_gross_exposure)
    {
      out.push_back({"max_gross_exposure", gross, *_rules.max_gross_exposure,
                     "gross " + std::to_string(gross) + " over cap " + std::to_string(*_rules.max_gross_exposure)});
    }
  }
  if (_rules.max_concentration_pct.has_value() && _accounts.size() >= 2)
  {
    const double total = totalGrossLocked();
    if (total > 0.0)
    {
      double max_share = 0.0;
      std::string max_name;
      for (const auto& [n, a] : _accounts)
      {
        const double share = a.gross_exposure / total;
        if (share > max_share)
        {
          max_share = share;
          max_name = n;
        }
      }
      if (max_share > *_rules.max_concentration_pct)
      {
        out.push_back({"max_concentration_pct", max_share,
                       *_rules.max_concentration_pct,
                       max_name + " holds " + std::to_string(max_share * 100.0) + "% of gross"});
      }
    }
  }
  return out;
}

void PortfolioRiskAggregator::reevaluateLocked()
{
  const double cur = currentEquityLocked();
  if (cur > _peak_equity)
  {
    _peak_equity = cur;
  }
  if (!_kill_switch_active && !breachesLocked().empty())
  {
    _kill_switch_active = true;
  }
}

PortfolioSnapshot PortfolioRiskAggregator::buildSnapshotLocked() const
{
  PortfolioSnapshot s;
  for (const auto& [_, a] : _accounts)
  {
    s.total_realized_pnl += a.realized_pnl;
    s.total_unrealized_pnl += a.unrealized_pnl;
    s.total_fees += a.fees;
    s.total_gross_exposure += a.gross_exposure;
    s.total_net_exposure += a.net_exposure;
    s.total_trade_count += a.trade_count;
    s.accounts.push_back(a);
  }
  s.total_daily_pnl = s.total_realized_pnl + s.total_unrealized_pnl + s.total_fees;
  s.current_equity = _initial_equity + s.total_daily_pnl;
  s.peak_equity = _peak_equity;
  s.drawdown_pct = drawdownPctLocked();
  s.kill_switch_active = _kill_switch_active;
  s.active_breaches = breachesLocked();
  return s;
}

}  // namespace flox::risk
