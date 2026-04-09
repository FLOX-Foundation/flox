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
  _currentEquity = Volume::fromDouble(_config.initialCapital);
  _peakEquity = _currentEquity;
}

void BacktestResult::recordFill(const Fill& fill)
{
  _fills.push_back(fill);

  const Volume fee = computeFee(fill.price, fill.quantity);
  _totalFees = Volume::fromRaw(_totalFees.raw() + fee.raw());

  Position& pos = getPosition(fill.symbol);

  if (fill.side == Side::BUY)
  {
    if (pos.quantity.raw() < 0)
    {
      const Quantity closeQty =
          Quantity::fromRaw(std::min(-pos.quantity.raw(), fill.quantity.raw()));
      const Volume pnl = computePnl(pos.avgPrice, fill.price, closeQty, false);
      recordTrade(fill.symbol, Side::BUY, pnl, fee, fill.timestampNs);
    }
    updatePositionLong(pos, fill.quantity, fill.price);
  }
  else
  {
    if (pos.quantity.raw() > 0)
    {
      const Quantity closeQty =
          Quantity::fromRaw(std::min(pos.quantity.raw(), fill.quantity.raw()));
      const Volume pnl = computePnl(pos.avgPrice, fill.price, closeQty, true);
      recordTrade(fill.symbol, Side::SELL, pnl, fee, fill.timestampNs);
    }
    updatePositionShort(pos, fill.quantity, fill.price);
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
    if (trade.pnl.raw() > 0)
    {
      stats.winningTrades++;
      grossProfitRaw += trade.pnl.raw();
    }
    else if (trade.pnl.raw() < 0)
    {
      stats.losingTrades++;
      grossLossRaw += -trade.pnl.raw();
    }
  }

  stats.totalPnl = _totalPnl.toDouble();
  stats.totalFees = _totalFees.toDouble();
  stats.netPnl = stats.totalPnl - stats.totalFees;
  stats.grossProfit = Volume::fromRaw(grossProfitRaw).toDouble();
  stats.grossLoss = Volume::fromRaw(grossLossRaw).toDouble();

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

  stats.maxDrawdown = _maxDrawdown.toDouble();
  if (_peakEquity.raw() > 0)
  {
    stats.maxDrawdownPct =
        static_cast<double>(_maxDrawdown.raw()) / static_cast<double>(_peakEquity.raw()) * 100.0;
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
  return _totalPnl.toDouble();
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

Volume BacktestResult::computePnl(Price entryPrice, Price exitPrice, Quantity qty, bool isLong)
{
  // Uses Price * Quantity -> Volume which is __int128-safe via common.h
  if (isLong)
  {
    const Price diff = Price::fromRaw(exitPrice.raw() - entryPrice.raw());
    return diff * qty;
  }
  else
  {
    const Price diff = Price::fromRaw(entryPrice.raw() - exitPrice.raw());
    return diff * qty;
  }
}

void BacktestResult::updatePositionLong(Position& pos, Quantity qty, Price price)
{
  if (pos.quantity.raw() >= 0)
  {
    // Weighted average: (avgPrice * pos + price * qty) / (pos + qty)
    // Uses Volume = Price * Quantity (safe) then Volume / Quantity -> Price (safe)
    const Volume existingValue = pos.avgPrice * pos.quantity;
    const Volume addedValue = price * qty;
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() + qty.raw());
    if (pos.quantity.raw() != 0)
    {
      const Volume totalValue = Volume::fromRaw(existingValue.raw() + addedValue.raw());
      pos.avgPrice = totalValue / pos.quantity;
    }
  }
  else
  {
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() + qty.raw());
    if (pos.quantity.raw() > 0)
    {
      pos.avgPrice = price;
    }
  }
}

void BacktestResult::updatePositionShort(Position& pos, Quantity qty, Price price)
{
  if (pos.quantity.raw() <= 0)
  {
    const Quantity absPos = Quantity::fromRaw(-pos.quantity.raw());
    const Volume existingValue = pos.avgPrice * absPos;
    const Volume addedValue = price * qty;
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() - qty.raw());
    if (pos.quantity.raw() != 0)
    {
      const Quantity newAbsPos = Quantity::fromRaw(-pos.quantity.raw());
      const Volume totalValue = Volume::fromRaw(existingValue.raw() + addedValue.raw());
      pos.avgPrice = totalValue / newAbsPos;
    }
  }
  else
  {
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() - qty.raw());
    if (pos.quantity.raw() < 0)
    {
      pos.avgPrice = price;
    }
  }
}

void BacktestResult::recordTrade(SymbolId symbol, Side side, Volume pnl, Volume fee,
                                 UnixNanos timestampNs)
{
  TradeRecord trade;
  trade.symbol = symbol;
  trade.side = side;
  trade.pnl = pnl;
  trade.fee = fee;
  trade.exitTimeNs = timestampNs;
  _trades.push_back(trade);

  const int64_t netPnlRaw = pnl.raw() - fee.raw();
  _totalPnl = Volume::fromRaw(_totalPnl.raw() + pnl.raw());
  _currentEquity = Volume::fromRaw(_currentEquity.raw() + netPnlRaw);

  if (_currentEquity.raw() > _peakEquity.raw())
  {
    _peakEquity = _currentEquity;
  }

  const int64_t drawdownRaw = _peakEquity.raw() - _currentEquity.raw();
  if (drawdownRaw > _maxDrawdown.raw())
  {
    _maxDrawdown = Volume::fromRaw(drawdownRaw);
  }
}

Volume BacktestResult::computeFee(Price price, Quantity qty) const
{
  if (_config.usePercentageFee)
  {
    // price * qty -> Volume (safe via __int128)
    const Volume notional = price * qty;
    return Volume::fromRaw(static_cast<int64_t>(notional.toDouble() * _config.feeRate *
                                                static_cast<double>(Volume::Scale)));
  }
  else
  {
    return Volume::fromDouble(_config.fixedFeePerTrade);
  }
}

double BacktestResult::computeSharpeRatio() const
{
  if (_trades.size() < 2)
  {
    return 0.0;
  }

  const size_t n = _trades.size();
  double sum = 0.0;
  double sumSq = 0.0;

  for (const auto& trade : _trades)
  {
    const double pnl = trade.pnl.toDouble();
    sum += pnl;
    sumSq += pnl * pnl;
  }

  const double mean = sum / static_cast<double>(n);
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
  double sum = 0.0;
  double downsideSumSq = 0.0;
  size_t downsideCount = 0;

  for (const auto& trade : _trades)
  {
    const double pnl = trade.pnl.toDouble();
    sum += pnl;
    if (pnl < 0)
    {
      downsideSumSq += pnl * pnl;
      ++downsideCount;
    }
  }

  if (downsideCount == 0)
  {
    return 0.0;
  }

  const double mean = sum / static_cast<double>(n);
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
  if (_trades.empty() || _maxDrawdown.raw() <= 0)
  {
    return 0.0;
  }

  const double totalReturn = _totalPnl.toDouble();
  const double maxDD = _maxDrawdown.toDouble();

  if (maxDD <= 0)
  {
    return 0.0;
  }

  return totalReturn / maxDD;
}

}  // namespace flox
