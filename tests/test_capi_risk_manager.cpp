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
  std::vector<uint8_t> allow_order_types;
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
  s->allow_order_types.push_back(sig->order_type);
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

TEST(CapiRiskManager, OrderTypeUsesTheWireEncodingNotTheCxxEnum)
{
  // FloxSignal.order_type must carry the FLOX_SIGNAL_TYPE_* codes on
  // every path that fills the struct. flox::OrderType disagrees on 0
  // and 1 (OrderType::LIMIT == 0, but the wire code for LIMIT is 1), so
  // a raw cast used to hand risk managers a market order tagged as a
  // limit order.
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

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  ASSERT_EQ(state.allow_order_types.size(), 1u);
  EXPECT_EQ(state.allow_order_types[0], FLOX_SIGNAL_TYPE_MARKET);

  flox_emit_limit_buy(s.strategy, s.symbol_id, flox_price_from_double(100.0),
                      flox_quantity_from_double(1.0));
  ASSERT_EQ(state.allow_order_types.size(), 2u);
  EXPECT_EQ(state.allow_order_types[1], FLOX_SIGNAL_TYPE_LIMIT);

  flox_runner_stop(s.runner);
  flox_risk_manager_destroy(rm);
}

namespace
{
// Captures FloxSignal.order_type as seen by a BACKTEST-path risk
// manager. That path converts an Order (not a Signal) via
// orderToFloxSignal, which is where the encoding bug lived.
std::vector<uint8_t> g_backtest_order_types;

uint8_t backtest_allow_cb(void*, const FloxSignal* sig)
{
  g_backtest_order_types.push_back(sig->order_type);
  return 1;
}
}  // namespace

TEST(CapiRiskManager, BacktestPathOrderTypeUsesTheWireEncoding)
{
  // The backtest hooks (risk manager / kill switch / order validator /
  // PnL tracker) receive an Order converted to FloxSignal. A raw cast of
  // flox::OrderType put LIMIT==0 and MARKET==1 into a field the wire
  // contract defines as MARKET==0, LIMIT==1, so a risk manager checking
  // for market orders saw limit orders and vice versa.
  g_backtest_order_types.clear();

  auto* registry = flox_registry_create();
  uint32_t sym = flox_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* btr = flox_backtest_runner_create(registry, 0.0, 10000.0);

  FloxRiskManagerCallbacks rcb{};
  rcb.allow = backtest_allow_cb;
  FloxRiskManagerHandle rm = flox_risk_manager_create(rcb);
  ASSERT_NE(rm, nullptr);
  flox_backtest_runner_set_risk_manager(btr, rm);

  static std::atomic<bool> fired{false};
  fired.store(false);
  struct St
  {
    FloxStrategyHandle h;
    uint32_t sym;
  } st{nullptr, sym};
  auto on_bar = +[](void* ud, const FloxSymbolContext*, const FloxBarData*)
  {
    if (fired.exchange(true))
    {
      return;
    }
    auto* p = static_cast<St*>(ud);
    flox_emit_market_buy(p->h, p->sym, 100000000LL);
  };
  FloxStrategyCallbacks scb{};
  scb.on_bar = on_bar;
  scb.user_data = &st;
  uint32_t syms[] = {sym};
  auto* strat = flox_strategy_create(1, syms, 1, registry, scb);
  st.h = strat;
  flox_backtest_runner_set_strategy(btr, strat);

  int64_t starts[1] = {1'000'000'000};
  int64_t ends[1] = {1'999'999'999};
  double opens[1] = {100.0}, highs[1] = {101.0}, lows[1] = {99.0};
  double closes[1] = {100.5}, volumes[1] = {10.0};
  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_bars(btr, starts, ends, opens, highs, lows,
                                         closes, volumes, 1, "BTC", 0, 0, &stats);
  EXPECT_EQ(rc, 1);

  ASSERT_FALSE(g_backtest_order_types.empty())
      << "risk manager never saw the emitted market order";
  EXPECT_EQ(g_backtest_order_types[0], FLOX_SIGNAL_TYPE_MARKET);

  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
  flox_risk_manager_destroy(rm);
}
