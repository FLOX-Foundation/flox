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
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace flox
{

class LatencyModel;

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
  FULL,
  // Pure pro-rata: every order at the price level receives a share
  // of the trade proportional to its size. Use on venues where the
  // matching engine distributes a trade across all orders at the
  // best price (most options venues; some hybrid spot venues).
  PRO_RATA,
  // FIFO top-N, then pro-rata across the remainder. Models hybrid
  // venues that reward queue front but distribute the rest.
  PRO_RATA_WITH_FIFO,
  // CME TOP-PRO-LMM (Globex options): the order at the front of the
  // queue ("TOP") receives a fixed share of the incoming trade (e.g.
  // 40%), capped by its remaining size. The remainder distributes
  // pro-rata, with LMM (Lead Market Maker) orders receiving a bonus
  // priority multiplier. Use `setTopPriorityShare`,
  // `setLmmOrders`, and `setOrderPriorityMultiplier` to configure.
  TOP_PRO_LMM,
  // ICE-style size-pro-rata-with-priority-multiplier: every order at
  // the level gets effective weight `remaining × priorityMultiplier`,
  // distributed proportionally. Per-order multiplier defaults to 1.0
  // (set via `setOrderPriorityMultiplier`).
  PRO_RATA_WITH_PRIORITY,
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

  // Per-side percentage rates. An empty optional (the default) means "not
  // set": both sides fall back to feeRate, which is why that field had to be
  // documented as a maker/taker average. Set either to charge the real spread
  // between the two -- on a venue where the maker rate is a rebate, a strategy
  // that only posts can be profitable at an average rate that says it is not.
  //
  // A negative rate is a rebate the venue pays, not a missing value. Reading
  // the sign as "unset" would make a rebate impossible to express, which is
  // exactly the rate a maker strategy is built around.
  //
  // Only consulted when usePercentageFee is true; Fill::isMaker selects the
  // side.
  std::optional<double> makerFeeRate{};
  std::optional<double> takerFeeRate{};

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

  // Replace ack model. Mirrors the cancel ack model. Zero (default)
  // preserves the legacy synchronous replace; positive values turn
  // replace async (REPLACE_SUBMITTED fires immediately, REPLACE_
  // ACCEPTED + REPLACED fire when the ack deadline elapses), and
  // REPLACE_REJECTED fires when the original order has already
  // filled within the ack window. Sampling shares cancelAckSeed.
  int64_t replaceAckLatencyNs{0};
  int64_t replaceAckJitterNs{0};

  // Submit ack model. Symmetric to cancel / replace. Zero (default)
  // preserves the legacy synchronous flow (SUBMITTED + ACCEPTED in
  // the same tick); positive values defer ACCEPTED until the
  // sampled deadline elapses. POST_ONLY orders that become
  // marketable during the ack window are REJECTED with
  // reject_reason = "late_post_only_crossed". The order does not
  // participate in queue matching or fills until ACCEPTED.
  int64_t submitAckLatencyNs{0};
  int64_t submitAckJitterNs{0};

  // Wire latency between the strategy and the matching engine. When set, an
  // order submitted at T is not marketable until T + orderDelay(), and its
  // fill is stamped at the time it actually matched -- the market can move
  // away in between, which is the whole point of modelling it. The delay is
  // drawn once per order and added to the venue's own submitAckLatencyNs, so
  // a model and an ack profile compose rather than override each other.
  //
  // Default null keeps the instant baseline, and so does a model that draws
  // zero: the deferral is decided by the sampled value, not by whether a
  // model is attached.
  //
  // Only orderDelay() is consumed today. feedDelay() and fillDelay() would
  // have to move event timestamps and callback delivery, which the runner and
  // the executor do not defer yet -- see docs/how-to/backtest-with-latency.md.
  //
  // Copied by value with the config, so every fold of a walk-forward run
  // shares the one model (and, for a stochastic one, its RNG stream).
  std::shared_ptr<LatencyModel> latency{};

  double riskFreeRate{0.0};
  // Annualization factor for Sharpe/Sortino/Calmar. NOTE: the equity curve is
  // sampled once PER CLOSED TRADE, not per calendar day, so 252 treats the
  // series as if each trade were one trading day. Set this to your strategy's
  // actual trades-per-year for a meaningful annualization, or leave 252 for a
  // per-trade-normalized comparison across runs.
  double metricsAnnualizationFactor{252.0};
};

}  // namespace flox
