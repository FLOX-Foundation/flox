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
#include <stdexcept>

namespace flox::risk
{

PortfolioRiskAggregator::PortfolioRiskAggregator(RiskRules rules,
                                                 double initial_equity)
    : _rules(std::move(rules)),
      _initial_equity(initial_equity),
      _peak_equity(initial_equity)
{
  if (_rules.max_drawdown_pct.has_value() && !(initial_equity > 0.0))
  {
    throw std::invalid_argument(
        "portfolio risk: max_drawdown_pct needs a positive initial_equity to "
        "measure against");
  }
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
    row.net_exposure_reported = true;
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

std::optional<Breach> PortfolioRiskAggregator::checkOrder(const std::string& strategy,
                                                          double notional,
                                                          const std::string& side) const
{
  std::lock_guard<std::mutex> guard(_mutex);
  if (_kill_switch_active)
  {
    return Breach{"kill_switch_active", 1.0, 0.0,
                  "portfolio kill switch is engaged"};
  }

  // Direction first, because every limit below honours it. A "reduce" is
  // explicit. A "sell" moves net exposure down and a "buy" up, read against
  // the net this strategy has reported; when it has never reported one, its
  // gross exposure stands in, so a caller who only feeds gross still gets
  // risk-reducing orders through. Those orders have to pass: near a limit is
  // exactly where you need them most, and the earlier code blocked them on
  // every state rule.
  auto it = _accounts.find(strategy);
  const bool haveRow = (it != _accounts.end());
  const double absNotional = std::abs(notional);

  bool reducesExposure = false;
  if (side == "reduce" || side == "REDUCE")
  {
    reducesExposure = true;
  }
  else if (haveRow)
  {
    const double delta = (side == "sell" || side == "SELL") ? -absNotional : absNotional;
    if (it->second.net_exposure_reported)
    {
      const double curNet = it->second.net_exposure;
      reducesExposure = std::abs(curNet + delta) < std::abs(curNet);
    }
    else
    {
      reducesExposure = (delta < 0.0) && (it->second.gross_exposure > 0.0);
    }
  }

  // State-based limits are pre-trade gates too, not just post-fill kill-switch
  // triggers: refuse a new exposure-increasing order once the book is already
  // in breach. The comparison matches breachesLocked() exactly, so the gate
  // and the snapshot never disagree; they used to differ by one unit in the
  // last place, and inside that window orders were rejected while snapshot()
  // showed no breach at all and the operator had nothing to read.
  if (!reducesExposure && _rules.max_daily_loss.has_value())
  {
    const double pnl = totalDailyPnlLocked();
    // The sign of the cap carries no meaning: -10,000 and 10,000 both mean a
    // ceiling of 10,000 on the loss, as breachesLocked() has always read it.
    // Taken literally, a negative cap rejected every order from a flat start
    // and only released once the day was 10,001 in profit, which is not
    // reachable without trading.
    const double cap = std::abs(*_rules.max_daily_loss);
    if (pnl < 0.0 && std::abs(pnl) > cap)
    {
      return Breach{"max_daily_loss", pnl, -cap,
                    "daily PnL " + std::to_string(pnl) + " below allowed loss " + std::to_string(-cap)};
    }
  }
  if (!reducesExposure && _rules.max_drawdown_pct.has_value())
  {
    const double dd = drawdownPctLocked();
    if (dd > *_rules.max_drawdown_pct)
    {
      return Breach{"max_drawdown_pct", dd, *_rules.max_drawdown_pct,
                    "drawdown " + std::to_string(dd) + " over cap " + std::to_string(*_rules.max_drawdown_pct)};
    }
  }

  if (!reducesExposure && _rules.max_gross_exposure.has_value())
  {
    const double cap = *_rules.max_gross_exposure;
    const double proposed = totalGrossLocked() + absNotional;
    if (proposed > cap)
    {
      return Breach{"max_gross_exposure", proposed, cap,
                    "order would push gross exposure to " + std::to_string(proposed) + " (cap " + std::to_string(cap) + ")"};
    }
  }

  // Concentration needs two contributing strategies to mean anything, the
  // same guard breachesLocked() applies. One strategy trading alone is 100%
  // of the book by definition, and without this the gate rejected every
  // opening order a single-strategy desk ever sent while snapshot() reported
  // a clean book.
  if (!reducesExposure && _rules.max_concentration_pct.has_value())
  {
    const double stratGross = (haveRow ? it->second.gross_exposure : 0.0) + absNotional;
    size_t contributors = contributingAccountsLocked();
    if (!haveRow || it->second.gross_exposure <= 0.0)
    {
      ++contributors;  // this order would make the candidate a contributor
    }

    const double total = totalGrossLocked() + absNotional;
    if (contributors >= 2 && total > 0.0)
    {
      const double conc = stratGross / total;
      if (conc > *_rules.max_concentration_pct)
      {
        return Breach{"max_concentration_pct", conc, *_rules.max_concentration_pct,
                      "strategy concentration " + std::to_string(conc) + " over cap " + std::to_string(*_rules.max_concentration_pct)};
      }
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

size_t PortfolioRiskAggregator::contributingAccountsLocked() const
{
  size_t n = 0;
  for (const auto& [_, a] : _accounts)
  {
    if (a.gross_exposure > 0.0)
    {
      ++n;
    }
  }
  return n;
}

double PortfolioRiskAggregator::drawdownPctLocked() const
{
  if (_peak_equity <= 0)
  {
    return 0.0;
  }
  const double cur = currentEquityLocked();
  // Clamped at 1.0. Unclamped, a small positive peak against a large loss
  // printed values like 500001.0 -- "50,000,100%" -- straight into
  // snapshot().drawdown_pct, the breach text and every report downstream.
  return std::clamp((_peak_equity - cur) / _peak_equity, 0.0, 1.0);
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
  // Contributing strategies, not registered rows. Counting rows meant that
  // when one of two strategies closed out and went flat, the other was
  // suddenly holding "100% of gross" and the kill switch halted the whole
  // portfolio -- on a normal flattening, clearable only by hand, and armed
  // again on the next update.
  if (_rules.max_concentration_pct.has_value() && contributingAccountsLocked() >= 2)
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
    s.accounts.push_back(static_cast<const StrategyAccount&>(a));
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
