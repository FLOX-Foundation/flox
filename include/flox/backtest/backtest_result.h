/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/simulated_executor.h"
#include "flox/common.h"
#include "flox/util/base/time.h"

#include <array>
#include <vector>

namespace flox
{

struct BacktestConfig
{
  double initialCapital{100000.0};
  double feeRate{0.0001};  // 0.01% per trade (maker/taker average)
  bool usePercentageFee{true};
  double fixedFeePerTrade{0.0};
};

struct TradeRecord
{
  SymbolId symbol{};
  Side side{};
  int64_t entryPriceRaw{0};
  int64_t exitPriceRaw{0};
  int64_t quantityRaw{0};
  UnixNanos entryTimeNs{0};
  UnixNanos exitTimeNs{0};
  int64_t pnlRaw{0};
  int64_t feeRaw{0};
};

struct BacktestStats
{
  size_t totalTrades{0};
  size_t winningTrades{0};
  size_t losingTrades{0};

  double initialCapital{0.0};
  double finalCapital{0.0};
  double totalPnl{0.0};
  double totalFees{0.0};
  double netPnl{0.0};
  double grossProfit{0.0};
  double grossLoss{0.0};

  double maxDrawdown{0.0};
  double maxDrawdownPct{0.0};

  double winRate{0.0};
  double profitFactor{0.0};
  double avgWin{0.0};
  double avgLoss{0.0};

  double sharpeRatio{0.0};
  double returnPct{0.0};

  UnixNanos startTimeNs{0};
  UnixNanos endTimeNs{0};
};

class BacktestResult
{
 public:
  static constexpr size_t kDefaultCapacity = 16384;
  static constexpr size_t kMaxSymbols = 256;

  explicit BacktestResult(const BacktestConfig& config = {}, size_t expectedFills = kDefaultCapacity);

  void recordFill(const Fill& fill);
  BacktestStats computeStats() const noexcept;

  const BacktestConfig& config() const noexcept { return _config; }

  const std::vector<Fill>& fills() const noexcept { return _fills; }
  const std::vector<TradeRecord>& trades() const noexcept { return _trades; }
  double totalPnl() const noexcept;

 private:
  struct Position
  {
    int64_t quantityRaw{0};
    int64_t avgPriceRaw{0};
  };

  Position& getPosition(SymbolId symbol) noexcept;

  static int64_t computePnlRaw(int64_t entryPriceRaw, int64_t exitPriceRaw, int64_t qtyRaw,
                               bool isLong) noexcept;

  void updatePositionLong(Position& pos, int64_t qtyRaw, int64_t priceRaw) noexcept;
  void updatePositionShort(Position& pos, int64_t qtyRaw, int64_t priceRaw) noexcept;
  void recordTrade(SymbolId symbol, Side side, int64_t pnlRaw, int64_t feeRaw,
                   UnixNanos timestampNs) noexcept;
  int64_t computeFeeRaw(int64_t priceRaw, int64_t qtyRaw) const noexcept;
  double computeSharpeRatio() const noexcept;

  BacktestConfig _config;

  std::vector<Fill> _fills;
  std::vector<TradeRecord> _trades;

  std::array<Position, kMaxSymbols> _positionsFlat{};
  std::vector<std::pair<SymbolId, Position>> _positionsOverflow;

  int64_t _totalPnlRaw{0};
  int64_t _totalFeesRaw{0};
  int64_t _currentEquityRaw{0};
  int64_t _peakEquityRaw{0};
  int64_t _maxDrawdownRaw{0};
};

}  // namespace flox
