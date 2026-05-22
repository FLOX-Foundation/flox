/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/latency_profiles.h"

#include <gtest/gtest.h>

using namespace flox;

TEST(LatencyProfiles, BinanceUmFuturesAppliesAllThreeKnobs)
{
  BacktestConfig cfg{};
  LatencyProfiles::binance_um_futures(cfg);
  EXPECT_GT(cfg.submitAckLatencyNs, 0);
  EXPECT_GT(cfg.cancelAckLatencyNs, 0);
  EXPECT_GT(cfg.replaceAckLatencyNs, 0);
  EXPECT_GT(cfg.submitAckJitterNs, 0);
  EXPECT_GT(cfg.cancelAckJitterNs, 0);
  EXPECT_GT(cfg.replaceAckJitterNs, 0);
}

TEST(LatencyProfiles, BybitLinearDiffersFromBinance)
{
  BacktestConfig a{};
  LatencyProfiles::binance_um_futures(a);
  BacktestConfig b{};
  LatencyProfiles::bybit_linear(b);
  // Different venues should produce different defaults (else why have a library).
  EXPECT_NE(a.submitAckLatencyNs, b.submitAckLatencyNs);
}

TEST(LatencyProfiles, IdealizedZerosEverything)
{
  BacktestConfig cfg{};
  // Set non-zero first.
  cfg.submitAckLatencyNs = 999;
  cfg.cancelAckLatencyNs = 999;
  cfg.replaceAckLatencyNs = 999;
  LatencyProfiles::idealized(cfg);
  EXPECT_EQ(cfg.submitAckLatencyNs, 0);
  EXPECT_EQ(cfg.cancelAckLatencyNs, 0);
  EXPECT_EQ(cfg.replaceAckLatencyNs, 0);
  EXPECT_EQ(cfg.submitAckJitterNs, 0);
  EXPECT_EQ(cfg.cancelAckJitterNs, 0);
  EXPECT_EQ(cfg.replaceAckJitterNs, 0);
}

TEST(LatencyProfiles, AdversarialFarLargerThanCalibrated)
{
  BacktestConfig calib{};
  LatencyProfiles::binance_um_futures(calib);
  BacktestConfig adv{};
  LatencyProfiles::adversarial(adv);
  EXPECT_GT(adv.submitAckLatencyNs, calib.submitAckLatencyNs * 5);
  EXPECT_GT(adv.cancelAckLatencyNs, calib.cancelAckLatencyNs * 5);
  EXPECT_GT(adv.replaceAckLatencyNs, calib.replaceAckLatencyNs * 5);
}

TEST(LatencyProfiles, AllSixProfilesCallable)
{
  BacktestConfig cfg{};
  LatencyProfiles::binance_um_futures(cfg);
  LatencyProfiles::bybit_linear(cfg);
  LatencyProfiles::okx_swap(cfg);
  LatencyProfiles::deribit(cfg);
  LatencyProfiles::idealized(cfg);
  LatencyProfiles::adversarial(cfg);
  // Just verify no crash; the last profile applied is adversarial.
  EXPECT_GT(cfg.submitAckLatencyNs, 0);
}
