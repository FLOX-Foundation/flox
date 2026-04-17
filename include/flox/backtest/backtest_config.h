/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace flox
{

enum class SlippageModel : uint8_t
{
  NONE,
  FIXED_TICKS,
  FIXED_BPS,
  VOLUME_IMPACT
};

enum class QueueModel : uint8_t
{
  NONE,
  TOB,
  FULL
};

struct SlippageProfile
{
  SlippageModel model{SlippageModel::NONE};
  int32_t ticks{0};         // FIXED_TICKS: ticks against the taker
  double bps{0.0};          // FIXED_BPS: basis points against the taker
  double impactCoeff{0.0};  // VOLUME_IMPACT: price_move = coeff * (orderQty / levelQty)
};

struct BacktestConfig
{
  double initialCapital{100000.0};
  double feeRate{0.0001};  // 0.01% per trade (maker/taker average)
  bool usePercentageFee{true};
  double fixedFeePerTrade{0.0};

  SlippageProfile defaultSlippage{};
  std::vector<std::pair<SymbolId, SlippageProfile>> perSymbolSlippage{};

  QueueModel queueModel{QueueModel::NONE};
  size_t queueDepth{8};

  double riskFreeRate{0.0};
  double metricsAnnualizationFactor{252.0};  // daily-equivalent sampling
};

}  // namespace flox
