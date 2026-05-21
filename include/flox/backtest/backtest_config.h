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
  Price tickSize{};         // FIXED_TICKS: price per tick; zero means 1 raw unit
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

  // Minimum fractional change in `queueAhead` required to emit a
  // QUEUE_POSITION_UPDATED event. 0.0 fires on every change (lossless,
  // very chatty on liquid books); 1.0 disables emission entirely. A
  // typical value is 0.05 (5%). Computed against the order's
  // `aheadAtArrival` so a single threshold applies to all sizes.
  double queuePositionMinChangeFraction{0.05};

  // Cancellation ack model. cancelAckLatencyNs is the base round-trip
  // delay between cancelOrder() and CANCELED firing. Zero (default)
  // preserves the legacy synchronous behavior. Positive values turn
  // cancels async: PENDING_CANCEL fires immediately, CANCELED fires
  // when sim time reaches `now + sampled_latency`, and the order may
  // still fill in that window (late-cancel-after-fill race).
  // cancelAckJitterNs adds a uniform jitter band; sampled latency is
  // drawn from [base - jitter, base + jitter]. cancelAckSeed makes
  // the sampling reproducible across runs.
  int64_t cancelAckLatencyNs{0};
  int64_t cancelAckJitterNs{0};
  uint64_t cancelAckSeed{42};

  double riskFreeRate{0.0};
  double metricsAnnualizationFactor{252.0};  // daily-equivalent sampling
};

}  // namespace flox
