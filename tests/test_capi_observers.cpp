/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the post-emission observer hooks (T018 PnLTracker, T019
// StorageSink). Both fire after the user on_signal callback, never block.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <vector>

namespace
{

struct State
{
  std::atomic<int> on_signal_calls{0};
  std::atomic<int> pnl_calls{0};
  std::atomic<int> storage_calls{0};

  // Order of callback invocations: 'u' = user on_signal, 'p' = pnl,
  // 's' = storage. Used to verify post-emission ordering.
  std::vector<char> order;
};

void on_signal_cb(void* ud, const FloxSignal*)
{
  auto* s = static_cast<State*>(ud);
  s->on_signal_calls.fetch_add(1, std::memory_order_relaxed);
  s->order.push_back('u');
}

void pnl_cb(void* ud, const FloxSignal*)
{
  auto* s = static_cast<State*>(ud);
  s->pnl_calls.fetch_add(1, std::memory_order_relaxed);
  s->order.push_back('p');
}

void storage_cb(void* ud, const FloxSignal*)
{
  auto* s = static_cast<State*>(ud);
  s->storage_calls.fetch_add(1, std::memory_order_relaxed);
  s->order.push_back('s');
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

  s.runner = flox_runner_create(s.registry, on_signal_cb, &state);
  ASSERT_NE(s.runner, nullptr);
  flox_runner_add_strategy(s.runner, s.strategy);
  flox_runner_start(s.runner);
}

}  // namespace

TEST(CapiObservers, PnLTrackerFiresAfterOnSignal)
{
  State state;
  RunnerCtx s;
  make_runner(s, state);

  FloxPnLTrackerCallbacks cb{};
  cb.on_signal = pnl_cb;
  cb.user_data = &state;
  FloxPnLTrackerHandle tracker = flox_pnl_tracker_create(cb);
  ASSERT_NE(tracker, nullptr);
  flox_runner_set_pnl_tracker(s.runner, tracker);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.on_signal_calls.load(), 1);
  EXPECT_EQ(state.pnl_calls.load(), 1);
  ASSERT_EQ(state.order.size(), 2u);
  EXPECT_EQ(state.order[0], 'u') << "user on_signal must fire before pnl";
  EXPECT_EQ(state.order[1], 'p');

  flox_runner_stop(s.runner);
  flox_pnl_tracker_destroy(tracker);
}

TEST(CapiObservers, StorageSinkFiresAfterOnSignal)
{
  State state;
  RunnerCtx s;
  make_runner(s, state);

  FloxStorageSinkCallbacks cb{};
  cb.store = storage_cb;
  cb.user_data = &state;
  FloxStorageSinkHandle sink = flox_storage_sink_create(cb);
  flox_runner_set_storage_sink(s.runner, sink);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.on_signal_calls.load(), 1);
  EXPECT_EQ(state.storage_calls.load(), 1);
  ASSERT_EQ(state.order.size(), 2u);
  EXPECT_EQ(state.order[0], 'u');
  EXPECT_EQ(state.order[1], 's');

  flox_runner_stop(s.runner);
  flox_storage_sink_destroy(sink);
}

TEST(CapiObservers, PnLBeforeStorage)
{
  // When both observers attached, the order should be:
  //   on_signal → PnL → Storage.
  State state;
  RunnerCtx s;
  make_runner(s, state);

  FloxPnLTrackerCallbacks pcb{};
  pcb.on_signal = pnl_cb;
  pcb.user_data = &state;
  FloxStorageSinkCallbacks scb{};
  scb.store = storage_cb;
  scb.user_data = &state;

  auto* tracker = flox_pnl_tracker_create(pcb);
  auto* sink = flox_storage_sink_create(scb);
  flox_runner_set_pnl_tracker(s.runner, tracker);
  flox_runner_set_storage_sink(s.runner, sink);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  ASSERT_EQ(state.order.size(), 3u);
  EXPECT_EQ(state.order[0], 'u');
  EXPECT_EQ(state.order[1], 'p');
  EXPECT_EQ(state.order[2], 's');

  flox_runner_stop(s.runner);
  flox_storage_sink_destroy(sink);
  flox_pnl_tracker_destroy(tracker);
}

TEST(CapiObservers, ObserversNeverBlock)
{
  // Even if an observer "would deny" by some custom logic, the user
  // on_signal callback already fired. Verify that returning from the
  // observer with no effect doesn't surface anywhere.
  State state;
  RunnerCtx s;
  make_runner(s, state);

  FloxPnLTrackerCallbacks pcb{};
  pcb.on_signal = pnl_cb;
  pcb.user_data = &state;
  auto* tracker = flox_pnl_tracker_create(pcb);
  flox_runner_set_pnl_tracker(s.runner, tracker);

  // 3 emissions; each fires user + pnl, regardless of "what happens" in
  // the observer.
  for (int i = 0; i < 3; ++i)
  {
    flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  }
  EXPECT_EQ(state.on_signal_calls.load(), 3);
  EXPECT_EQ(state.pnl_calls.load(), 3);

  flox_runner_stop(s.runner);
  flox_pnl_tracker_destroy(tracker);
}

TEST(CapiObservers, NullDetaches)
{
  State state;
  RunnerCtx s;
  make_runner(s, state);

  FloxPnLTrackerCallbacks pcb{};
  pcb.on_signal = pnl_cb;
  pcb.user_data = &state;
  auto* tracker = flox_pnl_tracker_create(pcb);
  flox_runner_set_pnl_tracker(s.runner, tracker);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.pnl_calls.load(), 1);

  flox_runner_set_pnl_tracker(s.runner, nullptr);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.pnl_calls.load(), 1) << "pnl detached; should not fire";
  EXPECT_EQ(state.on_signal_calls.load(), 2) << "user on_signal still works";

  flox_runner_stop(s.runner);
  flox_pnl_tracker_destroy(tracker);
}

TEST(CapiObservers, NullCallbackFunctionIsNoOp)
{
  State state;
  RunnerCtx s;
  make_runner(s, state);

  FloxPnLTrackerCallbacks cb{};
  cb.on_signal = nullptr;  // missing fn, but bundle present
  cb.user_data = &state;
  auto* tracker = flox_pnl_tracker_create(cb);
  flox_runner_set_pnl_tracker(s.runner, tracker);

  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));

  EXPECT_EQ(state.on_signal_calls.load(), 1);
  EXPECT_EQ(state.pnl_calls.load(), 0);

  flox_runner_stop(s.runner);
  flox_pnl_tracker_destroy(tracker);
}

TEST(CapiObservers, RiskDenialSkipsObservers)
{
  // If a pre-trade gate denies, the user on_signal does NOT fire — and
  // observers also must not fire (they're "after on_signal", not "after
  // emission attempt").
  State state;
  std::atomic<uint8_t> deny{0};

  auto allow_cb = [](void* ud, const FloxSignal*) -> uint8_t
  {
    return static_cast<std::atomic<uint8_t>*>(ud)->load(std::memory_order_acquire);
  };

  RunnerCtx s;
  make_runner(s, state);

  FloxRiskManagerCallbacks rcb{};
  rcb.allow = allow_cb;
  rcb.user_data = &deny;
  auto* rm = flox_risk_manager_create(rcb);
  flox_runner_set_risk_manager(s.runner, rm);

  FloxPnLTrackerCallbacks pcb{};
  pcb.on_signal = pnl_cb;
  pcb.user_data = &state;
  auto* tracker = flox_pnl_tracker_create(pcb);
  flox_runner_set_pnl_tracker(s.runner, tracker);

  // deny=0 → drop
  deny.store(0);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.on_signal_calls.load(), 0);
  EXPECT_EQ(state.pnl_calls.load(), 0) << "observers must not fire when risk denies";

  // deny=1 → allow
  deny.store(1);
  flox_emit_market_buy(s.strategy, s.symbol_id, flox_quantity_from_double(1.0));
  EXPECT_EQ(state.on_signal_calls.load(), 1);
  EXPECT_EQ(state.pnl_calls.load(), 1);

  flox_runner_stop(s.runner);
  flox_pnl_tracker_destroy(tracker);
  flox_risk_manager_destroy(rm);
}
