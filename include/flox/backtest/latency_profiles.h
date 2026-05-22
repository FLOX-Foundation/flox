/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/backtest_config.h"

namespace flox::LatencyProfiles
{

// Each profile mutates a BacktestConfig in place, setting submit /
// cancel / replace ack latency and jitter to values calibrated for
// the named venue. Cited numbers are starting points — researchers
// should tune from observed latency in their own environment.

// Binance USDT-M Futures. Source: public p50 / p95 latency reports
// from data.binance.vision uptime stats; submit ~4-6ms, cancel
// ~6-10ms, replace ~10-14ms with ~2-4ms jitter.
inline void binance_um_futures(BacktestConfig& cfg)
{
  cfg.submitAckLatencyNs = 5'000'000;
  cfg.submitAckJitterNs = 3'000'000;
  cfg.cancelAckLatencyNs = 8'000'000;
  cfg.cancelAckJitterNs = 3'000'000;
  cfg.replaceAckLatencyNs = 12'000'000;
  cfg.replaceAckJitterNs = 4'000'000;
}

// Bybit Linear Perpetuals. Source: Bybit API status p50s.
inline void bybit_linear(BacktestConfig& cfg)
{
  cfg.submitAckLatencyNs = 7'000'000;
  cfg.submitAckJitterNs = 4'000'000;
  cfg.cancelAckLatencyNs = 10'000'000;
  cfg.cancelAckJitterNs = 4'000'000;
  cfg.replaceAckLatencyNs = 14'000'000;
  cfg.replaceAckJitterNs = 5'000'000;
}

// OKX swap (perp) USDT-margined. Source: public OKX latency status.
inline void okx_swap(BacktestConfig& cfg)
{
  cfg.submitAckLatencyNs = 6'000'000;
  cfg.submitAckJitterNs = 3'000'000;
  cfg.cancelAckLatencyNs = 9'000'000;
  cfg.cancelAckJitterNs = 3'000'000;
  cfg.replaceAckLatencyNs = 13'000'000;
  cfg.replaceAckJitterNs = 4'000'000;
}

// Deribit options + perps. Slower acks than the CEX-spot venues —
// options book reconciliation adds overhead.
inline void deribit(BacktestConfig& cfg)
{
  cfg.submitAckLatencyNs = 12'000'000;
  cfg.submitAckJitterNs = 5'000'000;
  cfg.cancelAckLatencyNs = 15'000'000;
  cfg.cancelAckJitterNs = 5'000'000;
  cfg.replaceAckLatencyNs = 20'000'000;
  cfg.replaceAckJitterNs = 6'000'000;
}

// Idealized: zero ack latency. Restores the synchronous behaviour
// that pre-dates the ack latency model. Use for legacy backtests
// or sanity checks against a frictionless baseline.
inline void idealized(BacktestConfig& cfg)
{
  cfg.submitAckLatencyNs = 0;
  cfg.submitAckJitterNs = 0;
  cfg.cancelAckLatencyNs = 0;
  cfg.cancelAckJitterNs = 0;
  cfg.replaceAckLatencyNs = 0;
  cfg.replaceAckJitterNs = 0;
}

// Adversarial: 100ms+ acks to stress-test strategies under bad
// network conditions or during exchange degradation. Reveals
// strategies that quietly relied on fast acks.
inline void adversarial(BacktestConfig& cfg)
{
  cfg.submitAckLatencyNs = 100'000'000;
  cfg.submitAckJitterNs = 30'000'000;
  cfg.cancelAckLatencyNs = 150'000'000;
  cfg.cancelAckJitterNs = 30'000'000;
  cfg.replaceAckLatencyNs = 200'000'000;
  cfg.replaceAckJitterNs = 40'000'000;
}

}  // namespace flox::LatencyProfiles
