/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/liquidation_engine.h"

#include <algorithm>
#include <cmath>

namespace flox
{

double LiquidationEngine::mmFractionFor(double notional) const
{
  if (_tiers.empty())
  {
    return 0.0;
  }
  double mm = _tiers.front().mmFraction;
  for (const auto& t : _tiers)
  {
    if (notional >= t.minNotional)
    {
      mm = t.mmFraction;
    }
    else
    {
      break;
    }
  }
  return mm;
}

LiquidationOutcome LiquidationEngine::onMark(SymbolId symbol, double markPrice)
{
  LiquidationOutcome out;
  if (_positions.empty() || markPrice <= 0.0)
  {
    return out;
  }

  // Step 1: collect every position on this symbol that's underwater.
  std::vector<size_t> underwater;
  for (size_t i = 0; i < _positions.size(); ++i)
  {
    const auto& p = _positions[i];
    if (p.symbol != symbol || p.quantity == 0.0)
    {
      continue;
    }
    const double notional = std::abs(p.quantity) * markPrice;
    const double mm = mmFractionFor(notional);
    const double mmReq = notional * mm;
    // Unrealised PnL at mark.
    const double upnl = p.quantity * (markPrice - p.entryPrice);
    if (p.equity + upnl < mmReq)
    {
      underwater.push_back(i);
    }
  }
  if (underwater.empty())
  {
    return out;
  }

  // Step 2: liquidate each underwater position at the slippage-
  // adjusted bankruptcy price; book the deficit, if any.
  double totalDeficit = 0.0;
  for (size_t idx : underwater)
  {
    const auto& p = _positions[idx];
    const double signQ = (p.quantity > 0.0) ? 1.0 : -1.0;
    // Adverse slippage walks the price against the closer.
    const double slip = _slippageBps / 10000.0;
    const double closePrice = markPrice * (1.0 - signQ * slip);
    const double realized = p.quantity * (closePrice - p.entryPrice);
    const double residualEquity = p.equity + realized;
    out.liquidated.push_back(p.accountId);
    ++_statLiquidations;
    ++out.liquidationsCount;
    if (residualEquity < 0.0)
    {
      totalDeficit += -residualEquity;
    }
  }
  // Remove liquidated positions from the book. Iterate descending
  // so erase indexes stay valid.
  for (auto it = underwater.rbegin(); it != underwater.rend(); ++it)
  {
    _positions.erase(_positions.begin() + static_cast<long>(*it));
  }

  if (totalDeficit <= 0.0)
  {
    return out;
  }

  // Step 3: insurance fund absorbs as much as it can.
  const double insurancePayment = std::min(totalDeficit, _insuranceFund);
  if (insurancePayment > 0.0)
  {
    _insuranceFund -= insurancePayment;
    totalDeficit -= insurancePayment;
    out.insuranceFundDelta -= insurancePayment;
    ++_statInsurancePayments;
    ++out.insurancePaymentsCount;
  }
  if (totalDeficit <= 0.0 || !_adlEnabled)
  {
    return out;
  }

  // Step 4: ADL. Rank opposite-side profitable positions by PnL
  // ratio (PnL / equity) and force-close from the top until the
  // deficit is absorbed.
  // The "opposite" side relative to the cascade: the liquidations
  // above were on positions that ran against the mark move, so
  // their cohort is one direction; ADL hits the cohort that
  // profited from the same move. We pick all profitable positions
  // on this symbol regardless of side: a long-direction cascade
  // means underwater shorts (PnL > 0 on rally), so the ADL pool is
  // longs in profit (also PnL > 0). The selection criterion is
  // simply "current PnL > 0 on this symbol".
  struct AdlCandidate
  {
    size_t idx;
    double pnlRatio;
    double upnl;
  };
  std::vector<AdlCandidate> candidates;
  for (size_t i = 0; i < _positions.size(); ++i)
  {
    const auto& p = _positions[i];
    if (p.symbol != symbol || p.quantity == 0.0)
    {
      continue;
    }
    const double upnl = p.quantity * (markPrice - p.entryPrice);
    if (upnl > 0.0)
    {
      const double ratio = (p.equity > 0.0) ? (upnl / p.equity) : upnl;
      candidates.push_back({i, ratio, upnl});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const AdlCandidate& a, const AdlCandidate& b)
            { return a.pnlRatio > b.pnlRatio; });

  std::vector<size_t> closeIdxs;
  for (const auto& c : candidates)
  {
    if (totalDeficit <= 0.0)
    {
      break;
    }
    out.adlClosedOut.push_back(_positions[c.idx].accountId);
    closeIdxs.push_back(c.idx);
    totalDeficit -= c.upnl;
    ++_statAdlCloseouts;
    ++out.adlCloseoutsCount;
  }
  // Remove ADL-closed positions descending.
  std::sort(closeIdxs.begin(), closeIdxs.end(), std::greater<size_t>());
  for (size_t i : closeIdxs)
  {
    _positions.erase(_positions.begin() + static_cast<long>(i));
  }

  return out;
}

LiquidationEngine LiquidationEngine::binance_um_futures()
{
  LiquidationEngine e;
  // Binance UM USDT-margined futures, BTCUSDT bracket ladder (sampled).
  e.addTier(0.0, 0.004);
  e.addTier(50'000.0, 0.005);
  e.addTier(250'000.0, 0.01);
  e.addTier(1'000'000.0, 0.025);
  e.addTier(10'000'000.0, 0.05);
  e.addTier(20'000'000.0, 0.10);
  e.setInsuranceFundCapital(900'000'000.0);  // approximate Binance fund
  e.setLiquidationSlippageBps(15.0);
  e.setAdlEnabled(true);
  return e;
}

LiquidationEngine LiquidationEngine::bybit_linear()
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.addTier(100'000.0, 0.01);
  e.addTier(500'000.0, 0.025);
  e.addTier(2'500'000.0, 0.05);
  e.setInsuranceFundCapital(100'000'000.0);
  e.setLiquidationSlippageBps(20.0);
  e.setAdlEnabled(true);
  return e;
}

LiquidationEngine LiquidationEngine::okx_swap()
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.addTier(50'000.0, 0.0075);
  e.addTier(500'000.0, 0.02);
  e.setInsuranceFundCapital(150'000'000.0);
  e.setLiquidationSlippageBps(20.0);
  e.setAdlEnabled(true);
  return e;
}

}  // namespace flox
