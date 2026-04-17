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
#include <fstream>
#include <vector>

namespace flox
{

BacktestResult::BacktestResult(const BacktestConfig& config, size_t expectedFills) : _config(config)
{
  if (expectedFills > 0)
  {
    _fills.reserve(expectedFills);
    _trades.reserve(expectedFills / 2);
    _equityCurve.reserve(expectedFills / 2);
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
      recordTrade(fill.symbol, Side::BUY, pos.avgPrice, fill.price, closeQty,
                  pos.entryTimeNs, fill.timestampNs, pnl, fee);
    }
    updatePositionLong(pos, fill.quantity, fill.price, fill.timestampNs);
  }
  else
  {
    if (pos.quantity.raw() > 0)
    {
      const Quantity closeQty =
          Quantity::fromRaw(std::min(pos.quantity.raw(), fill.quantity.raw()));
      const Volume pnl = computePnl(pos.avgPrice, fill.price, closeQty, true);
      recordTrade(fill.symbol, Side::SELL, pos.avgPrice, fill.price, closeQty,
                  pos.entryTimeNs, fill.timestampNs, pnl, fee);
    }
    updatePositionShort(pos, fill.quantity, fill.price, fill.timestampNs);
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
  size_t winStreak = 0;
  size_t lossStreak = 0;
  std::vector<double> durations;
  durations.reserve(_trades.size());

  for (const auto& trade : _trades)
  {
    if (trade.pnl.raw() > 0)
    {
      stats.winningTrades++;
      grossProfitRaw += trade.pnl.raw();
      ++winStreak;
      lossStreak = 0;
      if (winStreak > stats.maxConsecutiveWins)
      {
        stats.maxConsecutiveWins = winStreak;
      }
    }
    else if (trade.pnl.raw() < 0)
    {
      stats.losingTrades++;
      grossLossRaw += -trade.pnl.raw();
      ++lossStreak;
      winStreak = 0;
      if (lossStreak > stats.maxConsecutiveLosses)
      {
        stats.maxConsecutiveLosses = lossStreak;
      }
    }
    else
    {
      winStreak = 0;
      lossStreak = 0;
    }

    if (trade.exitTimeNs >= trade.entryTimeNs)
    {
      durations.push_back(
          static_cast<double>(trade.exitTimeNs - trade.entryTimeNs));
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

  if (stats.avgLoss > 0.0)
  {
    stats.avgWinLossRatio = stats.avgWin / stats.avgLoss;
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

  if (!durations.empty())
  {
    double sum = 0.0;
    double maxDur = 0.0;
    for (double d : durations)
    {
      sum += d;
      if (d > maxDur)
      {
        maxDur = d;
      }
    }
    stats.avgTradeDurationNs = sum / static_cast<double>(durations.size());
    stats.maxTradeDurationNs = maxDur;
    std::vector<double> sorted = durations;
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    stats.medianTradeDurationNs = (sorted.size() % 2 == 0)
                                      ? 0.5 * (sorted[mid - 1] + sorted[mid])
                                      : sorted[mid];
  }

  stats.sharpeRatio = computeSharpeRatio();
  stats.sortinoRatio = computeSortinoRatio();
  stats.timeWeightedReturn = computeTimeWeightedReturn();
  stats.calmarRatio = computeCalmarRatio(stats.timeWeightedReturn);

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

bool BacktestResult::writeEquityCurveCsv(const std::string& path) const
{
  std::ofstream out(path);
  if (!out)
  {
    return false;
  }
  out << "timestamp_ns,equity,drawdown_pct\n";
  for (const auto& p : _equityCurve)
  {
    out << static_cast<int64_t>(p.timestampNs) << ',' << p.equity << ','
        << p.drawdownPct << '\n';
  }
  return static_cast<bool>(out);
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

void BacktestResult::updatePositionLong(Position& pos, Quantity qty, Price price,
                                        UnixNanos timestampNs)
{
  if (pos.quantity.raw() >= 0)
  {
    const bool wasFlat = (pos.quantity.raw() == 0);
    const Volume existingValue = pos.avgPrice * pos.quantity;
    const Volume addedValue = price * qty;
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() + qty.raw());
    if (pos.quantity.raw() != 0)
    {
      const Volume totalValue = Volume::fromRaw(existingValue.raw() + addedValue.raw());
      pos.avgPrice = totalValue / pos.quantity;
    }
    if (wasFlat)
    {
      pos.entryTimeNs = timestampNs;
    }
  }
  else
  {
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() + qty.raw());
    if (pos.quantity.raw() > 0)
    {
      pos.avgPrice = price;
      pos.entryTimeNs = timestampNs;
    }
    else if (pos.quantity.raw() == 0)
    {
      pos.entryTimeNs = 0;
    }
  }
}

void BacktestResult::updatePositionShort(Position& pos, Quantity qty, Price price,
                                         UnixNanos timestampNs)
{
  if (pos.quantity.raw() <= 0)
  {
    const bool wasFlat = (pos.quantity.raw() == 0);
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
    if (wasFlat)
    {
      pos.entryTimeNs = timestampNs;
    }
  }
  else
  {
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() - qty.raw());
    if (pos.quantity.raw() < 0)
    {
      pos.avgPrice = price;
      pos.entryTimeNs = timestampNs;
    }
    else if (pos.quantity.raw() == 0)
    {
      pos.entryTimeNs = 0;
    }
  }
}

void BacktestResult::recordTrade(SymbolId symbol, Side side, Price entryPrice, Price exitPrice,
                                 Quantity quantity, UnixNanos entryTimeNs, UnixNanos exitTimeNs,
                                 Volume pnl, Volume fee)
{
  TradeRecord trade;
  trade.symbol = symbol;
  trade.side = side;
  trade.entryPrice = entryPrice;
  trade.exitPrice = exitPrice;
  trade.quantity = quantity;
  trade.entryTimeNs = entryTimeNs;
  trade.exitTimeNs = exitTimeNs;
  trade.pnl = pnl;
  trade.fee = fee;
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

  EquityPoint pt;
  pt.timestampNs = exitTimeNs;
  pt.equity = _currentEquity.toDouble();
  const double peak = _peakEquity.toDouble();
  pt.drawdownPct = (peak > 0.0) ? (drawdownRaw / static_cast<double>(_peakEquity.raw())) * 100.0 : 0.0;
  _equityCurve.push_back(pt);
}

Volume BacktestResult::computeFee(Price price, Quantity qty) const
{
  if (_config.usePercentageFee)
  {
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
  if (_trades.size() < 2 || _config.initialCapital <= 0.0)
  {
    return 0.0;
  }

  const size_t n = _trades.size();
  const double capital = _config.initialCapital;
  double sum = 0.0;
  double sumSq = 0.0;

  for (const auto& trade : _trades)
  {
    const double ret = (trade.pnl.toDouble() / capital) - _config.riskFreeRate;
    sum += ret;
    sumSq += ret * ret;
  }

  const double mean = sum / static_cast<double>(n);
  const double meanSq = sumSq / static_cast<double>(n);
  const double variance = meanSq - mean * mean;

  if (variance <= 0)
  {
    return 0.0;
  }

  const double stddev = std::sqrt(variance);
  const double annualization = std::sqrt(_config.metricsAnnualizationFactor);
  return (mean / stddev) * annualization;
}

double BacktestResult::computeSortinoRatio() const
{
  if (_trades.size() < 2 || _config.initialCapital <= 0.0)
  {
    return 0.0;
  }

  const size_t n = _trades.size();
  const double capital = _config.initialCapital;
  double sum = 0.0;
  double downsideSumSq = 0.0;
  size_t downsideCount = 0;

  for (const auto& trade : _trades)
  {
    const double ret = (trade.pnl.toDouble() / capital) - _config.riskFreeRate;
    sum += ret;
    if (ret < 0)
    {
      downsideSumSq += ret * ret;
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

  const double annualization = std::sqrt(_config.metricsAnnualizationFactor);
  return (mean / downsideStddev) * annualization;
}

double BacktestResult::computeCalmarRatio(double annualizedReturn) const
{
  if (_trades.empty() || _maxDrawdown.raw() <= 0 || _config.initialCapital <= 0.0)
  {
    return 0.0;
  }

  const double maxDDPct =
      static_cast<double>(_maxDrawdown.raw()) / static_cast<double>(_peakEquity.raw());
  if (maxDDPct <= 0.0)
  {
    return 0.0;
  }

  // Calmar = annualized return / max drawdown (as percent fractions).
  return annualizedReturn / maxDDPct;
}

double BacktestResult::computeTimeWeightedReturn() const
{
  // TWR chains per-period returns computed from consecutive equity points.
  // Returns the cumulative product minus 1 (i.e. total period return).
  if (_equityCurve.empty() || _config.initialCapital <= 0.0)
  {
    return 0.0;
  }

  double prev = _config.initialCapital;
  double product = 1.0;
  for (const auto& pt : _equityCurve)
  {
    if (prev <= 0.0)
    {
      break;
    }
    const double r = (pt.equity - prev) / prev;
    product *= (1.0 + r);
    prev = pt.equity;
  }
  return product - 1.0;
}

}  // namespace flox
