/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Subject for the strategy-side raw best-quote cases: a strategy whose book
// for symbol 1 holds a bid at exactly 0.0 and which has no book at all for
// symbol 0 (symbol ids start at 1, so 0 is the id no symbol ever gets). Those
// are the two states flox_best_bid_raw cannot tell apart, and the two the
// _opt accessors have to.
//
// It is built through the C API alone, along the path a live strategy takes:
// registry, strategy, runner, one snapshot. Force-included into the test
// target (see tests/CMakeLists.txt) so the test source stays C API + gtest.

#pragma once

#include "flox/capi/flox_capi.h"

namespace flox_test_support
{

inline void onSignalIgnored(void*, const FloxSignal*) {}

struct StrategyUnderTest
{
  FloxRegistryHandle registry{nullptr};
  FloxStrategyHandle strategy{nullptr};
  FloxRunnerHandle runner{nullptr};

  StrategyUnderTest()
  {
    registry = flox_registry_create();
    const uint32_t atZero = flox_registry_add_symbol(registry, "test", "ZERO", 1.0);

    FloxStrategyCallbacks callbacks{};
    uint32_t symbols[] = {atZero};
    strategy = flox_strategy_create(1, symbols, 1, registry, callbacks);

    runner = flox_runner_create(registry, onSignalIgnored, nullptr);
    flox_runner_add_strategy(runner, strategy);
    flox_runner_start(runner);

    double bidPrices[] = {0.0};
    double bidQtys[] = {1.0};
    flox_runner_on_book_snapshot(runner, atZero, bidPrices, bidQtys, 1, nullptr, nullptr, 0,
                                 /*exchange_ts_ns=*/1);

    flox_runner_stop(runner);
  }

  ~StrategyUnderTest()
  {
    flox_runner_destroy(runner);
    flox_strategy_destroy(strategy);
    flox_registry_destroy(registry);
  }

  StrategyUnderTest(const StrategyUnderTest&) = delete;
  StrategyUnderTest& operator=(const StrategyUnderTest&) = delete;
};

}  // namespace flox_test_support

inline FloxStrategyHandle currentStrategyUnderTest()
{
  static flox_test_support::StrategyUnderTest subject;
  return subject.strategy;
}
