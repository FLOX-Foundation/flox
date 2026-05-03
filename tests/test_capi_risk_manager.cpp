/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the C-API risk-manager hook.
//
// The hook is a callback bundle attached to a runner (or live engine) via
// flox_runner_set_risk_manager / flox_live_engine_set_risk_manager. Its
// `allow` function fires synchronously on every signal a strategy emits;
// returning 0 drops the signal before it reaches the user's on_signal,
// returning non-zero lets it through.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <vector>

namespace
{

// Plumbing: track signal events received by user callbacks vs allow checks.
struct State
{
  std::atomic<int> signals_received{0};
  std::atomic<int> allow_calls{0};
  std::atomic<uint8_t> allow_return{1};  // 1 = allow, 0 = deny
  std::vector<uint64_t> received_order_ids;
  std::vector<uint64_t> allow_order_ids;
};

void on_signal(void* ud, const FloxSignal* sig)
{
  auto* s = static_cast<State*>(ud);
  s->signals_received.fetch_add(1, std::memory_order_relaxed);
  s->received_order_ids.push_back(sig->order_id);
}

uint8_t allow_cb(void* ud, const FloxSignal* sig)
{
  auto* s = static_cast<State*>(ud);
  s->allow_calls.fetch_add(1, std::memory_order_relaxed);
  s->allow_order_ids.push_back(sig->order_id);
  return s->allow_return.load(std::memory_order_acquire);
}

// Build a minimal runner with one strategy bound to a single symbol so
// the strategy can emit market orders. Returns owned handles which the
// caller must destroy in reverse order.
struct RunnerCtx
{
  FloxRegistryHandle registry{nullptr};
  FloxStrategyHandle strategy{nullptr};
  FloxRunnerHandle runner{nullptr};
  uint32_t symbol_id{0};

  ~RunnerCtx()
  {
    if (runner)
      flox_runner_destroy(runner);
    if (strategy)
      flox_strategy_destroy(strategy);
    if (registry)
      flox_registry_destroy(registry);
  }
};

void make_setup(RunnerCtx& s, State& state)
{
  s.registry = flox_registry_create();
  ASSERT_NE(s.registry, nullptr);
  s.symbol_id = flox_registry_add_symbol(s.registry, "test", "BTC", 0.01);

  // Strategy with a no-op callback bundle. We exercise emit_market_buy
  // directly from the test, so the strategy callbacks themselves don't
  // need to do anything.
  FloxStrategyCallbacks cb{};
  uint32_t syms[] = {s.symbol_id};
  s.strategy = flox_strategy_create(/*id=*/1, syms, 1, s.registry, cb);
  ASSERT_NE(s.strategy, nullptr);

  s.runner = flox_runner_create(s.registry, on_signal, &state);
  ASSERT_NE(s.runner, nullptr);
  flox_runner_add_strategy(s.runner, s.strategy);
  flox_runner_start(s.runner);
}

}  // namespace

TEST(CapiRiskManager, NullRiskManagerLetsAllSignalsThrough)
{
  State state;
  RunnerCtx s;
  make_setup(s, state);

  uint64_t order_id =
      flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_NE(order_id, 0u);
  EXPECT_EQ(state.signals_received.load(), 1);
  EXPECT_EQ(state.allow_calls.load(), 0);
  ASSERT_EQ(state.received_order_ids.size(), 1u);
  EXPECT_EQ(state.received_order_ids[0], order_id);

  flox_runner_stop(s.runner);
}

TEST(CapiRiskManager, AllowReturnsOneLetsSignalThrough)
{
  State state;
  state.allow_return.store(1);

  RunnerCtx s;
  make_setup(s, state);

  FloxRiskManagerCallbacks cb{};
  cb.allow = allow_cb;
  cb.user_data = &state;
  FloxRiskManagerHandle rm = flox_risk_manager_create(cb);
  ASSERT_NE(rm, nullptr);
  flox_runner_set_risk_manager(s.runner, rm);

  uint64_t order_id =
      flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_NE(order_id, 0u);
  EXPECT_EQ(state.allow_calls.load(), 1);
  EXPECT_EQ(state.signals_received.load(), 1);
  ASSERT_EQ(state.allow_order_ids.size(), 1u);
  EXPECT_EQ(state.allow_order_ids[0], order_id);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
}

TEST(CapiRiskManager, AllowReturnsZeroDropsSignal)
{
  State state;
  state.allow_return.store(0);

  RunnerCtx s;
  make_setup(s, state);

  FloxRiskManagerCallbacks cb{};
  cb.allow = allow_cb;
  cb.user_data = &state;
  FloxRiskManagerHandle rm = flox_risk_manager_create(cb);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(2.0));

  // Both calls saw allow(), neither propagated to on_signal.
  EXPECT_EQ(state.allow_calls.load(), 2);
  EXPECT_EQ(state.signals_received.load(), 0);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
}

TEST(CapiRiskManager, DetachReenablesNormalFlow)
{
  State state;
  state.allow_return.store(0);  // start denying

  RunnerCtx s;
  make_setup(s, state);

  FloxRiskManagerCallbacks cb{};
  cb.allow = allow_cb;
  cb.user_data = &state;
  FloxRiskManagerHandle rm = flox_risk_manager_create(cb);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.signals_received.load(), 0);

  // Detach.
  flox_runner_set_risk_manager(s.runner, nullptr);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(2.0));
  EXPECT_EQ(state.signals_received.load(), 1)
      << "After detach, signals must propagate without consulting risk";
  EXPECT_EQ(state.allow_calls.load(), 1)
      << "Only the first emission should have triggered the (now-detached) hook";

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
}

TEST(CapiRiskManager, AllowReturnIsConsultedPerSignal)
{
  // Toggle allow_return between calls and verify each signal is judged
  // independently (i.e. there's no caching of the previous decision).
  State state;
  state.allow_return.store(1);

  RunnerCtx s;
  make_setup(s, state);

  FloxRiskManagerCallbacks cb{};
  cb.allow = allow_cb;
  cb.user_data = &state;
  FloxRiskManagerHandle rm = flox_risk_manager_create(cb);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  state.allow_return.store(0);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  state.allow_return.store(1);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.allow_calls.load(), 3);
  EXPECT_EQ(state.signals_received.load(), 2);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
}

TEST(CapiRiskManager, NullAllowFunctionLetsSignalThrough)
{
  // A risk manager whose `allow` function is NULL should be a no-op —
  // not a crash and not a deny.
  State state;

  RunnerCtx s;
  make_setup(s, state);

  FloxRiskManagerCallbacks cb{};
  cb.allow = nullptr;
  cb.user_data = &state;
  FloxRiskManagerHandle rm = flox_risk_manager_create(cb);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.signals_received.load(), 1);
  EXPECT_EQ(state.allow_calls.load(), 0);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
}
