/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_result.h"

#include <algorithm>
#include <cmath>

namespace flox
{

BacktestResult::BacktestResult(const BacktestConfig& config, size_t expectedFills) : _config(config)
{
  if (expectedFills > 0)
  {
    _fills.reserve(expectedFills);
    _trades.reserve(expectedFills / 2);
  }
  _currentEquityRaw = static_cast<int64_t>(_config.initialCapital * Price::Scale);
  _peakEquityRaw = _currentEquityRaw;
}

void BacktestResult::recordFill(const Fill& fill)
{
  _fills.push_back(fill);

  const int64_t fillPriceRaw = fill.price.raw();
  const int64_t fillQtyRaw = fill.quantity.raw();
  const int64_t feeRaw = computeFeeRaw(fillPriceRaw, fillQtyRaw);

  _totalFeesRaw += feeRaw;

  Position& pos = getPosition(fill.symbol);

  if (fill.side == Side::BUY)
  {
    if (pos.quantityRaw < 0)
    {
      const int64_t closeQtyRaw = std::min(-pos.quantityRaw, fillQtyRaw);
      const int64_t pnlRaw = computePnlRaw(pos.avgPriceRaw, fillPriceRaw, closeQtyRaw, false);
      recordTrade(fill.symbol, Side::BUY, pnlRaw, feeRaw, fill.timestampNs);
    }
    updatePositionLong(pos, fillQtyRaw, fillPriceRaw);
  }
  else
  {
    if (pos.quantityRaw > 0)
    {
      const int64_t closeQtyRaw = std::min(pos.quantityRaw, fillQtyRaw);
      const int64_t pnlRaw = computePnlRaw(pos.avgPriceRaw, fillPriceRaw, closeQtyRaw, true);
      recordTrade(fill.symbol, Side::SELL, pnlRaw, feeRaw, fill.timestampNs);
    }
    updatePositionShort(pos, fillQtyRaw, fillPriceRaw);
  }
}

BacktestStats BacktestResult::computeStats() const
{
  BacktestStats stats;
  stats.totalTrades = _trades.size();
  stats.initialCapital = _config.initialCapital;

  if (stats.totalTrades == 0)
  {
    stats.finalCapital = _config.initialCapital;
    return stats;
  }

  int64_t grossProfitRaw = 0;
  int64_t grossLossRaw = 0;

  for (const auto& trade : _trades)
  {
    if (trade.pnlRaw > 0)
    {
      stats.winningTrades++;
      grossProfitRaw += trade.pnlRaw;
    }
    else if (trade.pnlRaw < 0)
    {
      stats.losingTrades++;
      grossLossRaw += -trade.pnlRaw;
    }
  }

  constexpr double kScale = static_cast<double>(Price::Scale);

  stats.totalPnl = static_cast<double>(_totalPnlRaw) / kScale;
  stats.totalFees = static_cast<double>(_totalFeesRaw) / kScale;
  stats.netPnl = stats.totalPnl - stats.totalFees;
  stats.grossProfit = static_cast<double>(grossProfitRaw) / kScale;
  stats.grossLoss = static_cast<double>(grossLossRaw) / kScale;

  stats.finalCapital = _config.initialCapital + stats.netPnl;
  stats.returnPct = (stats.netPnl / _config.initialCapital) * 100.0;

  stats.winRate =
      static_cast<double>(stats.winningTrades) / static_cast<double>(stats.totalTrades);

  if (stats.winningTrades > 0)
  {
    stats.avgWin = stats.grossProfit / static_cast<double>(stats.winningTrades);
  }

  if (stats.losingTrades > 0)
  {
    stats.avgLoss = stats.grossLoss / static_cast<double>(stats.losingTrades);
  }

  if (grossLossRaw > 0)
  {
    stats.profitFactor = static_cast<double>(grossProfitRaw) / static_cast<double>(grossLossRaw);
  }

  stats.maxDrawdown = static_cast<double>(_maxDrawdownRaw) / kScale;
  if (_peakEquityRaw > 0)
  {
    stats.maxDrawdownPct =
        static_cast<double>(_maxDrawdownRaw) / static_cast<double>(_peakEquityRaw) * 100.0;
  }

  stats.sharpeRatio = computeSharpeRatio();
  stats.sortinoRatio = computeSortinoRatio();
  stats.calmarRatio = computeCalmarRatio();

  if (!_fills.empty())
  {
    stats.startTimeNs = _fills.front().timestampNs;
    stats.endTimeNs = _fills.back().timestampNs;
  }

  return stats;
}

double BacktestResult::totalPnl() const
{
  return static_cast<double>(_totalPnlRaw) / static_cast<double>(Price::Scale);
}

BacktestResult::Position& BacktestResult::getPosition(SymbolId symbol)
{
  if (symbol < kMaxSymbols) [[likely]]
  {
    return _positionsFlat[symbol];
  }

  for (auto& [id, pos] : _positionsOverflow)
  {
    if (id == symbol)
    {
      return pos;
    }
  }
  _positionsOverflow.emplace_back(symbol, Position{});
  return _positionsOverflow.back().second;
}

int64_t BacktestResult::computePnlRaw(int64_t entryPriceRaw, int64_t exitPriceRaw, int64_t qtyRaw,
                                      bool isLong)
{
  const int64_t diff = isLong ? (exitPriceRaw - entryPriceRaw) : (entryPriceRaw - exitPriceRaw);
  return (diff * qtyRaw) / Price::Scale;
}

void BacktestResult::updatePositionLong(Position& pos, int64_t qtyRaw, int64_t priceRaw)
{
  if (pos.quantityRaw >= 0)
  {
    const int64_t totalValueRaw =
        (pos.avgPriceRaw * pos.quantityRaw + priceRaw * qtyRaw) / Price::Scale;
    pos.quantityRaw += qtyRaw;
    if (pos.quantityRaw != 0)
    {
      pos.avgPriceRaw = (totalValueRaw * Price::Scale) / pos.quantityRaw;
    }
  }
  else
  {
    pos.quantityRaw += qtyRaw;
    if (pos.quantityRaw > 0)
    {
      pos.avgPriceRaw = priceRaw;
    }
  }
}

void BacktestResult::updatePositionShort(Position& pos, int64_t qtyRaw, int64_t priceRaw)
{
  if (pos.quantityRaw <= 0)
  {
    const int64_t totalValueRaw =
        (pos.avgPriceRaw * (-pos.quantityRaw) + priceRaw * qtyRaw) / Price::Scale;
    pos.quantityRaw -= qtyRaw;
    if (pos.quantityRaw != 0)
    {
      pos.avgPriceRaw = (totalValueRaw * Price::Scale) / (-pos.quantityRaw);
    }
  }
  else
  {
    pos.quantityRaw -= qtyRaw;
    if (pos.quantityRaw < 0)
    {
      pos.avgPriceRaw = priceRaw;
    }
  }
}

void BacktestResult::recordTrade(SymbolId symbol, Side side, int64_t pnlRaw, int64_t feeRaw,
                                 UnixNanos timestampNs)
{
  TradeRecord trade;
  trade.symbol = symbol;
  trade.side = side;
  trade.pnlRaw = pnlRaw;
  trade.feeRaw = feeRaw;
  trade.exitTimeNs = timestampNs;
  _trades.push_back(trade);

  const int64_t netPnlRaw = pnlRaw - feeRaw;
  _totalPnlRaw += pnlRaw;
  _currentEquityRaw += netPnlRaw;

  if (_currentEquityRaw > _peakEquityRaw)
  {
    _peakEquityRaw = _currentEquityRaw;
  }

  const int64_t drawdownRaw = _peakEquityRaw - _currentEquityRaw;
  if (drawdownRaw > _maxDrawdownRaw)
  {
    _maxDrawdownRaw = drawdownRaw;
  }
}

int64_t BacktestResult::computeFeeRaw(int64_t priceRaw, int64_t qtyRaw) const
{
  if (_config.usePercentageFee)
  {
    // fee = price * qty * feeRate / Scale (since price and qty are both scaled)
    const int64_t notionalRaw = (priceRaw * qtyRaw) / Price::Scale;
    return static_cast<int64_t>(static_cast<double>(notionalRaw) * _config.feeRate);
  }
  else
  {
    return static_cast<int64_t>(_config.fixedFeePerTrade * Price::Scale);
  }
}

double BacktestResult::computeSharpeRatio() const
{
  if (_trades.size() < 2)
  {
    return 0.0;
  }

  const size_t n = _trades.size();
  int64_t sum = 0;
  double sumSq = 0.0;

  for (const auto& trade : _trades)
  {
    sum += trade.pnlRaw;
    const double pnl = static_cast<double>(trade.pnlRaw);
    sumSq += pnl * pnl;
  }

  const double mean = static_cast<double>(sum) / static_cast<double>(n);
  const double meanSq = sumSq / static_cast<double>(n);
  const double variance = meanSq - mean * mean;

  if (variance <= 0)
  {
    return 0.0;
  }

  const double stddev = std::sqrt(variance);
  constexpr double kAnnualizationFactor = 15.8745;  // sqrt(252)

  return (mean / stddev) * kAnnualizationFactor;
}

double BacktestResult::computeSortinoRatio() const
{
  if (_trades.size() < 2)
  {
    return 0.0;
  }

  const size_t n = _trades.size();
  int64_t sum = 0;
  double downsideSumSq = 0.0;
  size_t downsideCount = 0;

  for (const auto& trade : _trades)
  {
    sum += trade.pnlRaw;
    if (trade.pnlRaw < 0)
    {
      const double pnl = static_cast<double>(trade.pnlRaw);
      downsideSumSq += pnl * pnl;
      ++downsideCount;
    }
  }

  // If no losing trades, Sortino is undefined - return 0
  if (downsideCount == 0)
  {
    return 0.0;
  }

  const double mean = static_cast<double>(sum) / static_cast<double>(n);

  // Downside deviation: sqrt of sum of squared negative returns / n
  const double downsideMeanSq = downsideSumSq / static_cast<double>(n);
  const double downsideStddev = std::sqrt(downsideMeanSq);

  if (downsideStddev <= 0)
  {
    return 0.0;
  }

  constexpr double kAnnualizationFactor = 15.8745;  // sqrt(252)
  return (mean / downsideStddev) * kAnnualizationFactor;
}

double BacktestResult::computeCalmarRatio() const
{
  if (_trades.empty() || _maxDrawdownRaw <= 0)
  {
    return 0.0;
  }

  // Calmar = Return / Max Drawdown (simple ratio, not annualized)
  constexpr double kScale = static_cast<double>(Price::Scale);
  const double totalReturn = static_cast<double>(_totalPnlRaw) / kScale;
  const double maxDrawdown = static_cast<double>(_maxDrawdownRaw) / kScale;

  if (maxDrawdown <= 0)
  {
    return 0.0;
  }

  return totalReturn / maxDrawdown;
}

}  // namespace flox
