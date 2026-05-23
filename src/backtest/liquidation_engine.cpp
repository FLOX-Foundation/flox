/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/liquidation_engine.h"

#include "flox/backtest/simulated_executor.h"
#include "flox/execution/order.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>

namespace flox
{

namespace
{
// Allocate synthetic order IDs for liquidation orders out of the
// top half of the OrderId space so they don't collide with
// strategy-issued IDs.
std::atomic<uint64_t> g_liquidationOrderIdCounter{1ULL << 62};
OrderId nextLiquidationOrderId()
{
  return g_liquidationOrderIdCounter.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

void LiquidationEngine::setAdlRankingByName(const std::string& name)
{
  std::string lower;
  lower.reserve(name.size());
  for (char c : name)
  {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "pnl_ratio" || lower == "pnlratio")
  {
    _adlRanking = AdlRanking::PnlRatio;
  }
  else if (lower == "binance")
  {
    _adlRanking = AdlRanking::Binance;
  }
  else if (lower == "bybit")
  {
    _adlRanking = AdlRanking::Bybit;
  }
  else if (lower == "position_size" || lower == "positionsize")
  {
    _adlRanking = AdlRanking::PositionSize;
  }
}

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

  // Step 2: liquidate each underwater position. With an executor
  // attached, route through it as a market order; without one,
  // fall back to the flat-bps slippage close.
  double totalDeficit = 0.0;
  std::vector<size_t> liquidated;
  liquidated.reserve(underwater.size());
  for (size_t idx : underwater)
  {
    auto& p = _positions[idx];
    double closePrice = 0.0;
    double filledQty = 0.0;
    if (_executor != nullptr)
    {
      const auto exClose = closeThroughExecutor(p, markPrice);
      closePrice = exClose.closePrice;
      filledQty = exClose.filledQty;
      if (filledQty <= 0.0)
      {
        // Executor couldn't fill any of this position this tick
        // (e.g. empty book). Leave the position in place; the next
        // onMark call will retry. Do not count as a liquidation.
        continue;
      }
    }
    else
    {
      const double signQ = (p.quantity > 0.0) ? 1.0 : -1.0;
      const double slip = _slippageBps / 10000.0;
      closePrice = markPrice * (1.0 - signQ * slip);
      filledQty = std::abs(p.quantity);
    }
    const double signedFilled = (p.quantity > 0.0) ? filledQty : -filledQty;
    const double realized = signedFilled * (closePrice - p.entryPrice);
    // Equity attributed to the filled portion (proportional).
    const double filledEquity = p.equity * (filledQty / std::abs(p.quantity));
    const double residualEquity = filledEquity + realized;
    out.liquidated.push_back(p.accountId);
    ++_statLiquidations;
    ++out.liquidationsCount;
    if (residualEquity < 0.0)
    {
      totalDeficit += -residualEquity;
    }
    // If the executor partial-filled, leave the unfilled remainder
    // in place for the next tick. Otherwise mark the whole position
    // for removal.
    if (filledQty >= std::abs(p.quantity) - 1e-12)
    {
      liquidated.push_back(idx);
    }
    else
    {
      const double remaining = std::abs(p.quantity) - filledQty;
      p.quantity = (p.quantity > 0.0) ? remaining : -remaining;
      p.equity -= filledEquity;
    }
  }
  // Remove fully-liquidated positions from the book. Iterate descending
  // so erase indexes stay valid.
  std::sort(liquidated.begin(), liquidated.end(), std::greater<size_t>());
  for (size_t i : liquidated)
  {
    _positions.erase(_positions.begin() + static_cast<long>(i));
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
    double score;
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
      double score = 0.0;
      switch (_adlRanking)
      {
        case AdlRanking::PnlRatio:
          score = (p.equity > 0.0) ? (upnl / p.equity) : upnl;
          break;
        case AdlRanking::Binance:
        case AdlRanking::Bybit:
        {
          const double notional = std::abs(p.quantity) * markPrice;
          const double leverage = (p.equity > 0.0) ? (notional / p.equity) : 0.0;
          score = upnl * leverage;
          break;
        }
        case AdlRanking::PositionSize:
          score = std::abs(p.quantity);
          break;
      }
      candidates.push_back({i, score, upnl});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const AdlCandidate& a, const AdlCandidate& b)
            { return a.score > b.score; });

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

LiquidationEngine::ExecutorClose LiquidationEngine::closeThroughExecutor(
    const LeveragedPosition& p, double markPrice)
{
  ExecutorClose out;
  if (_executor == nullptr || p.quantity == 0.0)
  {
    return out;
  }
  // Snapshot fill count before submitting; new fills above this
  // index are attributable to the liquidation order.
  const size_t fillsBefore = _executor->fills().size();
  Order order;
  order.id = nextLiquidationOrderId();
  order.symbol = p.symbol;
  // Side that closes the position.
  order.side = (p.quantity > 0.0) ? Side::SELL : Side::BUY;
  order.type = OrderType::MARKET;
  order.quantity = Quantity::fromDouble(std::abs(p.quantity));
  order.price = Price::fromDouble(markPrice);
  _executor->submitOrder(order);
  const auto& fills = _executor->fills();
  if (fills.size() <= fillsBefore)
  {
    return out;  // executor didn't fill anything (empty book)
  }
  // Aggregate fills attributable to this liquidation order.
  double totalQty = 0.0;
  double notional = 0.0;
  for (size_t i = fillsBefore; i < fills.size(); ++i)
  {
    const auto& f = fills[i];
    if (f.orderId != order.id)
    {
      continue;
    }
    const double q = f.quantity.toDouble();
    const double px = f.price.toDouble();
    totalQty += q;
    notional += q * px;
  }
  if (totalQty <= 0.0)
  {
    return out;
  }
  out.filledQty = totalQty;
  out.closePrice = notional / totalQty;
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
  e.setAdlRanking(AdlRanking::Binance);
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
  e.setAdlRanking(AdlRanking::Bybit);
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
  e.setAdlRanking(AdlRanking::PnlRatio);
  return e;
}

}  // namespace flox
