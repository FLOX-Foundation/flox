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

// A second subject, for the cases the one above cannot state: every book state
// the three _opt accessors have to answer differently, one symbol each, on one
// strategy. Tick size 1.0 throughout, so every price below lands on an exact
// tick and the expected raw values do not depend on how an off-tick price is
// snapped.
//
//   bidAtZero   bid 0.0, no ask    -- bid present at raw 0, ask and mid absent
//   askOnly     ask 5.0, no bid    -- ask present, bid and mid absent
//   askAtZero   bid -2.0, ask 0.0  -- ask present at raw 0
//   midAtZero   bid -1.0, ask 1.0  -- mid present at raw 0, from a live book
//   belowZero   bid -101, ask -99  -- all three present below zero
//   noSymbol    id 0               -- an id the registry never issues: no book
struct BookStatesUnderTest
{
  FloxRegistryHandle registry{nullptr};
  FloxStrategyHandle strategy{nullptr};
  FloxRunnerHandle runner{nullptr};

  uint32_t noSymbol{0};
  uint32_t bidAtZero{0};
  uint32_t askOnly{0};
  uint32_t askAtZero{0};
  uint32_t midAtZero{0};
  uint32_t belowZero{0};

  BookStatesUnderTest()
  {
    registry = flox_registry_create();
    bidAtZero = flox_registry_add_symbol(registry, "test", "BID-AT-ZERO", 1.0);
    askOnly = flox_registry_add_symbol(registry, "test", "ASK-ONLY", 1.0);
    askAtZero = flox_registry_add_symbol(registry, "test", "ASK-AT-ZERO", 1.0);
    midAtZero = flox_registry_add_symbol(registry, "test", "MID-AT-ZERO", 1.0);
    belowZero = flox_registry_add_symbol(registry, "test", "BELOW-ZERO", 1.0);

    FloxStrategyCallbacks callbacks{};
    uint32_t symbols[] = {bidAtZero, askOnly, askAtZero, midAtZero, belowZero};
    strategy = flox_strategy_create(1, symbols, 5, registry, callbacks);

    runner = flox_runner_create(registry, onSignalIgnored, nullptr);
    flox_runner_add_strategy(runner, strategy);
    flox_runner_start(runner);

    const double qty[] = {1.0};

    const double zero[] = {0.0};
    flox_runner_on_book_snapshot(runner, bidAtZero, zero, qty, 1, nullptr, nullptr, 0,
                                 /*exchange_ts_ns=*/1);

    const double five[] = {5.0};
    flox_runner_on_book_snapshot(runner, askOnly, nullptr, nullptr, 0, five, qty, 1, 1);

    const double minusTwo[] = {-2.0};
    flox_runner_on_book_snapshot(runner, askAtZero, minusTwo, qty, 1, zero, qty, 1, 1);

    const double minusOne[] = {-1.0};
    const double plusOne[] = {1.0};
    flox_runner_on_book_snapshot(runner, midAtZero, minusOne, qty, 1, plusOne, qty, 1, 1);

    const double minus101[] = {-101.0};
    const double minus99[] = {-99.0};
    flox_runner_on_book_snapshot(runner, belowZero, minus101, qty, 1, minus99, qty, 1, 1);

    flox_runner_stop(runner);
  }

  ~BookStatesUnderTest()
  {
    flox_runner_destroy(runner);
    flox_strategy_destroy(strategy);
    flox_registry_destroy(registry);
  }

  BookStatesUnderTest(const BookStatesUnderTest&) = delete;
  BookStatesUnderTest& operator=(const BookStatesUnderTest&) = delete;
};

}  // namespace flox_test_support

inline FloxStrategyHandle currentStrategyUnderTest()
{
  static flox_test_support::StrategyUnderTest subject;
  return subject.strategy;
}

inline const flox_test_support::BookStatesUnderTest& bookStatesUnderTest()
{
  static flox_test_support::BookStatesUnderTest subject;
  return subject;
}
