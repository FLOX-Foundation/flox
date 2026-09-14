/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the C-API kill-switch hook + the kill-switch / validator /
// risk-manager evaluation order.
//
// Order: KillSwitch (cheap) → OrderValidator (sanity) → RiskManager (logic).
// Each is optional. Returning 0 from any drops the signal and skips the
// remaining hooks.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <vector>

namespace
{

struct State
{
  std::atomic<int> signals_received{0};
  std::atomic<int> kill_calls{0};
  std::atomic<int> validate_calls{0};
  std::atomic<int> allow_calls{0};
  std::atomic<uint8_t> kill_return{1};
  std::atomic<uint8_t> validate_return{1};
  std::atomic<uint8_t> allow_return{1};
};

void on_signal(void* ud, const FloxSignal*)
{
  static_cast<State*>(ud)->signals_received.fetch_add(1, std::memory_order_relaxed);
}

uint8_t kill_cb(void* ud, const FloxSignal*)
{
  auto* s = static_cast<State*>(ud);
  s->kill_calls.fetch_add(1, std::memory_order_relaxed);
  return s->kill_return.load(std::memory_order_acquire);
}

uint8_t validate_cb(void* ud, const FloxSignal*)
{
  auto* s = static_cast<State*>(ud);
  s->validate_calls.fetch_add(1, std::memory_order_relaxed);
  return s->validate_return.load(std::memory_order_acquire);
}

uint8_t allow_cb(void* ud, const FloxSignal*)
{
  auto* s = static_cast<State*>(ud);
  s->allow_calls.fetch_add(1, std::memory_order_relaxed);
  return s->allow_return.load(std::memory_order_acquire);
}

struct RunnerCtx
{
  FloxRegistryHandle registry{nullptr};
  FloxStrategyHandle strategy{nullptr};
  FloxRunnerHandle runner{nullptr};
  uint32_t symbol_id{0};

  ~RunnerCtx()
  {
    if (runner)
    {
      flox_runner_destroy(runner);
    }
    if (strategy)
    {
      flox_strategy_destroy(strategy);
    }
    if (registry)
    {
      flox_registry_destroy(registry);
    }
  }
};

void make_runner(RunnerCtx& s, State& state)
{
  s.registry = flox_registry_create();
  ASSERT_NE(s.registry, nullptr);
  s.symbol_id = flox_registry_add_symbol(s.registry, "test", "BTC", 0.01);

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

// ── KillSwitch ───────────────────────────────────────────────────────

TEST(CapiKillSwitch, AllowedSignalPropagates)
{
  State state;
  state.kill_return.store(1);

  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks cb{};
  cb.check = kill_cb;
  cb.user_data = &state;
  FloxKillSwitchHandle ks = flox_kill_switch_create(cb);
  ASSERT_NE(ks, nullptr);
  flox_runner_set_kill_switch(s.runner, ks);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.signals_received.load(), 1);

  flox_runner_stop(s.runner);
  flox_kill_switch_destroy(ks);
}

TEST(CapiKillSwitch, TriggeredSignalDropped)
{
  State state;
  state.kill_return.store(0);

  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks cb{};
  cb.check = kill_cb;
  cb.user_data = &state;
  FloxKillSwitchHandle ks = flox_kill_switch_create(cb);
  flox_runner_set_kill_switch(s.runner, ks);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.signals_received.load(), 0);

  flox_runner_stop(s.runner);
  flox_kill_switch_destroy(ks);
}

TEST(CapiKillSwitch, NullCheckFunctionIsNoOp)
{
  State state;
  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks cb{};
  cb.check = nullptr;
  cb.user_data = &state;
  FloxKillSwitchHandle ks = flox_kill_switch_create(cb);
  flox_runner_set_kill_switch(s.runner, ks);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.kill_calls.load(), 0);
  EXPECT_EQ(state.signals_received.load(), 1);

  flox_runner_stop(s.runner);
  flox_kill_switch_destroy(ks);
}

// ── OrderValidator ───────────────────────────────────────────────────

TEST(CapiOrderValidator, ValidatedSignalPropagates)
{
  State state;
  state.validate_return.store(1);

  RunnerCtx s;
  make_runner(s, state);

  FloxOrderValidatorCallbacks cb{};
  cb.validate = validate_cb;
  cb.user_data = &state;
  FloxOrderValidatorHandle ov = flox_order_validator_create(cb);
  flox_runner_set_order_validator(s.runner, ov);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.validate_calls.load(), 1);
  EXPECT_EQ(state.signals_received.load(), 1);

  flox_runner_stop(s.runner);
  flox_order_validator_destroy(ov);
}

TEST(CapiOrderValidator, RejectedSignalDropped)
{
  State state;
  state.validate_return.store(0);

  RunnerCtx s;
  make_runner(s, state);

  FloxOrderValidatorCallbacks cb{};
  cb.validate = validate_cb;
  cb.user_data = &state;
  FloxOrderValidatorHandle ov = flox_order_validator_create(cb);
  flox_runner_set_order_validator(s.runner, ov);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.validate_calls.load(), 1);
  EXPECT_EQ(state.signals_received.load(), 0);

  flox_runner_stop(s.runner);
  flox_order_validator_destroy(ov);
}

// ── Evaluation order: kill → validator → risk → on_signal ────────────

TEST(CapiPreTradeHooks, AllThreePassThrough)
{
  State state;
  state.kill_return.store(1);
  state.validate_return.store(1);
  state.allow_return.store(1);

  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks kcb{};
  kcb.check = kill_cb;
  kcb.user_data = &state;
  FloxOrderValidatorCallbacks vcb{};
  vcb.validate = validate_cb;
  vcb.user_data = &state;
  FloxRiskManagerCallbacks rcb{};
  rcb.allow = allow_cb;
  rcb.user_data = &state;

  auto* ks = flox_kill_switch_create(kcb);
  auto* ov = flox_order_validator_create(vcb);
  auto* rm = flox_risk_manager_create(rcb);
  flox_runner_set_kill_switch(s.runner, ks);
  flox_runner_set_order_validator(s.runner, ov);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.validate_calls.load(), 1);
  EXPECT_EQ(state.allow_calls.load(), 1);
  EXPECT_EQ(state.signals_received.load(), 1);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
  flox_order_validator_destroy(ov);
  flox_kill_switch_destroy(ks);
}

TEST(CapiPreTradeHooks, KillSwitchShortCircuitsValidatorAndRisk)
{
  State state;
  state.kill_return.store(0);  // kill first
  state.validate_return.store(1);
  state.allow_return.store(1);

  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks kcb{};
  kcb.check = kill_cb;
  kcb.user_data = &state;
  FloxOrderValidatorCallbacks vcb{};
  vcb.validate = validate_cb;
  vcb.user_data = &state;
  FloxRiskManagerCallbacks rcb{};
  rcb.allow = allow_cb;
  rcb.user_data = &state;

  auto* ks = flox_kill_switch_create(kcb);
  auto* ov = flox_order_validator_create(vcb);
  auto* rm = flox_risk_manager_create(rcb);
  flox_runner_set_kill_switch(s.runner, ks);
  flox_runner_set_order_validator(s.runner, ov);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.validate_calls.load(), 0)
      << "OrderValidator must not run after KillSwitch denies";
  EXPECT_EQ(state.allow_calls.load(), 0)
      << "RiskManager must not run after KillSwitch denies";
  EXPECT_EQ(state.signals_received.load(), 0);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
  flox_order_validator_destroy(ov);
  flox_kill_switch_destroy(ks);
}

TEST(CapiPreTradeHooks, ValidatorShortCircuitsRisk)
{
  State state;
  state.kill_return.store(1);
  state.validate_return.store(0);  // validator denies
  state.allow_return.store(1);

  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks kcb{};
  kcb.check = kill_cb;
  kcb.user_data = &state;
  FloxOrderValidatorCallbacks vcb{};
  vcb.validate = validate_cb;
  vcb.user_data = &state;
  FloxRiskManagerCallbacks rcb{};
  rcb.allow = allow_cb;
  rcb.user_data = &state;

  auto* ks = flox_kill_switch_create(kcb);
  auto* ov = flox_order_validator_create(vcb);
  auto* rm = flox_risk_manager_create(rcb);
  flox_runner_set_kill_switch(s.runner, ks);
  flox_runner_set_order_validator(s.runner, ov);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.validate_calls.load(), 1);
  EXPECT_EQ(state.allow_calls.load(), 0)
      << "RiskManager must not run after OrderValidator denies";
  EXPECT_EQ(state.signals_received.load(), 0);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
  flox_order_validator_destroy(ov);
  flox_kill_switch_destroy(ks);
}

TEST(CapiPreTradeHooks, RiskManagerLastDeny)
{
  State state;
  state.kill_return.store(1);
  state.validate_return.store(1);
  state.allow_return.store(0);  // risk manager denies last

  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks kcb{};
  kcb.check = kill_cb;
  kcb.user_data = &state;
  FloxOrderValidatorCallbacks vcb{};
  vcb.validate = validate_cb;
  vcb.user_data = &state;
  FloxRiskManagerCallbacks rcb{};
  rcb.allow = allow_cb;
  rcb.user_data = &state;

  auto* ks = flox_kill_switch_create(kcb);
  auto* ov = flox_order_validator_create(vcb);
  auto* rm = flox_risk_manager_create(rcb);
  flox_runner_set_kill_switch(s.runner, ks);
  flox_runner_set_order_validator(s.runner, ov);
  flox_runner_set_risk_manager(s.runner, rm);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.validate_calls.load(), 1);
  EXPECT_EQ(state.allow_calls.load(), 1);
  EXPECT_EQ(state.signals_received.load(), 0);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
  flox_order_validator_destroy(ov);
  flox_kill_switch_destroy(ks);
}

TEST(CapiPreTradeHooks, IndependentlyDetachable)
{
  State state;
  state.kill_return.store(1);
  state.validate_return.store(1);
  state.allow_return.store(1);

  RunnerCtx s;
  make_runner(s, state);

  FloxKillSwitchCallbacks kcb{};
  kcb.check = kill_cb;
  kcb.user_data = &state;
  FloxOrderValidatorCallbacks vcb{};
  vcb.validate = validate_cb;
  vcb.user_data = &state;
  auto* ks = flox_kill_switch_create(kcb);
  auto* ov = flox_order_validator_create(vcb);

  flox_runner_set_kill_switch(s.runner, ks);
  flox_runner_set_order_validator(s.runner, ov);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.validate_calls.load(), 1);

  // Detach kill switch only.
  flox_runner_set_kill_switch(s.runner, nullptr);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.kill_calls.load(), 1) << "kill_switch was detached";
  EXPECT_EQ(state.validate_calls.load(), 2) << "validator still attached";

  // Detach validator too.
  flox_runner_set_order_validator(s.runner, nullptr);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.kill_calls.load(), 1);
  EXPECT_EQ(state.validate_calls.load(), 2) << "validator was detached";
  EXPECT_EQ(state.signals_received.load(), 3);

  flox_runner_stop(s.runner);
  flox_order_validator_destroy(ov);
  flox_kill_switch_destroy(ks);
}

// ============================================================================
// The backtest kill switch latches
// ============================================================================

namespace
{

struct LatchState
{
  int calls{0};
  FloxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
};

// Halt on the first order, allow everything after. A switch that latches
// stops the run at the first order; one that only caches its last answer
// resumes trading on the second.
uint8_t halt_once_cb(void* ud, const FloxSignal*)
{
  auto* s = static_cast<LatchState*>(ud);
  ++s->calls;
  return s->calls == 1 ? 0u : 1u;
}

void emit_alternating(void* ud, const FloxSymbolContext*, const FloxBarData*)
{
  auto* s = static_cast<LatchState*>(ud);
  static int bar = 0;
  if (bar++ % 2 == 0)
  {
    flox_emit_market_buy(s->strategy, s->symbol, 100000000LL);
  }
  else
  {
    flox_emit_market_sell(s->strategy, s->symbol, 100000000LL);
  }
}

}  // namespace

// The C ABI adapter assigned its triggered flag from each check instead of
// latching it, so an "allow" answer un-triggered a switch that had already
// said halt: five more orders executed and two round trips completed. The
// Python adapter for the same interface latches, which meant one strategy
// produced two different backtest results depending on the binding.
TEST(CapiKillSwitch, BacktestKillSwitchLatchesAfterTheFirstHalt)
{
  auto* registry = flox_registry_create();
  uint32_t sym = flox_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* btr = flox_backtest_runner_create(registry, 0.0, 10000.0);

  LatchState state;
  state.symbol = sym;

  FloxKillSwitchCallbacks kcb{};
  kcb.check = halt_once_cb;
  kcb.user_data = &state;
  FloxKillSwitchHandle ks = flox_kill_switch_create(kcb);
  ASSERT_NE(ks, nullptr);
  flox_backtest_runner_set_kill_switch(btr, ks);

  FloxStrategyCallbacks scb{};
  scb.on_bar = emit_alternating;
  scb.user_data = &state;
  uint32_t syms[] = {sym};
  auto* strat = flox_strategy_create(1, syms, 1, registry, scb);
  state.strategy = strat;
  flox_backtest_runner_set_strategy(btr, strat);

  constexpr uint32_t kBars = 6;
  int64_t starts[kBars];
  int64_t ends[kBars];
  double opens[kBars], highs[kBars], lows[kBars], closes[kBars], volumes[kBars];
  for (uint32_t i = 0; i < kBars; ++i)
  {
    starts[i] = static_cast<int64_t>(i + 1) * 1'000'000'000;
    ends[i] = starts[i] + 999'999'999;
    opens[i] = 100.0;
    highs[i] = 101.0;
    lows[i] = 99.0;
    closes[i] = 100.5;
    volumes[i] = 10.0;
  }

  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_bars(btr, starts, ends, opens, highs, lows,
                                         closes, volumes, kBars, "BTC", 0, 0, &stats);
  EXPECT_EQ(rc, 1);

  EXPECT_GE(state.calls, 1);
  EXPECT_EQ(stats.totalTrades, 0u)
      << "orders executed after the kill switch said halt";

  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
  flox_kill_switch_destroy(ks);
}
