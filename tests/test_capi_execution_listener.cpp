/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the ExecutionListener hook (T023). A binding-supplied
// listener observes order lifecycle events from BacktestRunner's
// SimulatedExecutor — fills, cancels, rejects, etc.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>

namespace
{

struct State
{
  std::atomic<int> submitted{0};
  std::atomic<int> accepted{0};
  std::atomic<int> partially_filled{0};
  std::atomic<int> filled{0};
  std::atomic<int> pending_cancel{0};
  std::atomic<int> canceled{0};
  std::atomic<int> expired{0};
  std::atomic<int> rejected{0};
  std::atomic<int> replaced{0};
  std::atomic<int> pending_trigger{0};
  std::atomic<int> triggered{0};
  std::atomic<int> trailing_updated{0};

  // Last seen order on filled — for sanity of payload packing.
  uint64_t last_filled_id{0};
  uint32_t last_filled_symbol{0};
  uint8_t last_filled_side{0};
  int64_t last_filled_price_raw{0};
  int64_t last_filled_qty_raw{0};

  // Last reason text from rejection.
  std::string last_reject_reason;
};

void on_submitted(void* ud, const FloxOrder*) { static_cast<State*>(ud)->submitted++; }
void on_accepted(void* ud, const FloxOrder*) { static_cast<State*>(ud)->accepted++; }
void on_partial(void* ud, const FloxOrder*, int64_t)
{
  static_cast<State*>(ud)->partially_filled++;
}
void on_filled(void* ud, const FloxOrder* o)
{
  auto* s = static_cast<State*>(ud);
  s->filled++;
  s->last_filled_id = o->id;
  s->last_filled_symbol = o->symbol;
  s->last_filled_side = o->side;
  s->last_filled_price_raw = o->price_raw;
  s->last_filled_qty_raw = o->quantity_raw;
}
void on_pending_cancel(void* ud, const FloxOrder*) { static_cast<State*>(ud)->pending_cancel++; }
void on_canceled(void* ud, const FloxOrder*) { static_cast<State*>(ud)->canceled++; }
void on_expired(void* ud, const FloxOrder*) { static_cast<State*>(ud)->expired++; }
void on_rejected(void* ud, const FloxOrder*, const char* reason)
{
  auto* s = static_cast<State*>(ud);
  s->rejected++;
  if (reason != nullptr)
  {
    s->last_reject_reason = reason;
  }
}
void on_replaced(void* ud, const FloxOrder*, const FloxOrder*)
{
  static_cast<State*>(ud)->replaced++;
}
void on_pending_trigger(void* ud, const FloxOrder*) { static_cast<State*>(ud)->pending_trigger++; }
void on_triggered(void* ud, const FloxOrder*) { static_cast<State*>(ud)->triggered++; }
void on_trailing_updated(void* ud, const FloxOrder*, int64_t)
{
  static_cast<State*>(ud)->trailing_updated++;
}

FloxExecutionListenerCallbacks make_cb(State& s)
{
  FloxExecutionListenerCallbacks cb{};
  cb.on_submitted = on_submitted;
  cb.on_accepted = on_accepted;
  cb.on_partially_filled = on_partial;
  cb.on_filled = on_filled;
  cb.on_pending_cancel = on_pending_cancel;
  cb.on_canceled = on_canceled;
  cb.on_expired = on_expired;
  cb.on_rejected = on_rejected;
  cb.on_replaced = on_replaced;
  cb.on_pending_trigger = on_pending_trigger;
  cb.on_triggered = on_triggered;
  cb.on_trailing_stop_updated = on_trailing_updated;
  cb.user_data = &s;
  return cb;
}

}  // namespace

TEST(CapiExecutionListener, CreateAndDestroy)
{
  State s;
  auto* listener = flox_execution_listener_create(make_cb(s));
  EXPECT_NE(listener, nullptr);
  flox_execution_listener_destroy(listener);
}

TEST(CapiExecutionListener, ReceivesFillsFromBacktestRunner)
{
  // Drive a market-buy strategy through a single-bar OHLCV backtest;
  // SimulatedExecutor fills the order at bar close and emits on_filled.
  State s;
  auto* listener = flox_execution_listener_create(make_cb(s));

  auto* registry = flox_registry_create();
  uint32_t sym = flox_registry_add_symbol(registry, "test", "BTC", 0.01);

  auto* btr = flox_backtest_runner_create(registry, /*fee=*/0.0,
                                          /*initial_capital=*/10000.0);
  flox_backtest_runner_add_execution_listener(btr, listener);

  // Strategy that fires a single market buy on the first trade.
  static std::atomic<bool> fired{false};
  fired.store(false);
  struct StratState
  {
    FloxStrategyHandle h;
    uint32_t sym;
  } sst{nullptr, sym};

  auto on_bar = +[](void* ud, const FloxSymbolContext* /*ctx*/, const FloxBarData* /*b*/)
  {
    if (fired.exchange(true))
    {
      return;
    }
    auto* st = static_cast<StratState*>(ud);
    flox_emit_market_buy(st->h, st->sym, /*qty_raw=*/100000000LL /* = 1.0 */);
  };

  FloxStrategyCallbacks scb{};
  scb.on_bar = on_bar;
  scb.user_data = &sst;
  uint32_t syms[] = {sym};
  auto* strat = flox_strategy_create(/*id=*/1, syms, 1, registry, scb);
  sst.h = strat;
  flox_backtest_runner_set_strategy(btr, strat);

  // Three bars; market-buy fires on first, fills as bars feed the executor.
  int64_t starts[3] = {1'000'000'000, 2'000'000'000, 3'000'000'000};
  int64_t ends[3] = {1'999'999'999, 2'999'999'999, 3'999'999'999};
  double opens[3] = {100.0, 101.0, 102.0};
  double highs[3] = {101.0, 102.0, 103.0};
  double lows[3] = {99.0, 100.5, 101.5};
  double closes[3] = {100.5, 101.5, 102.5};
  double volumes[3] = {10.0, 10.0, 10.0};

  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_bars(btr, starts, ends, opens, highs, lows,
                                         closes, volumes, 3, "BTC",
                                         /*bar_type=*/0, /*bar_type_param=*/0,
                                         &stats);
  EXPECT_EQ(rc, 1);

  EXPECT_GE(s.filled.load(), 1) << "market buy should fill at least once";
  EXPECT_EQ(s.last_filled_symbol, sym);
  EXPECT_EQ(s.last_filled_side, 0u) << "BUY → side=0";
  EXPECT_EQ(s.last_filled_qty_raw, 100000000LL);

  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
  flox_execution_listener_destroy(listener);
}

TEST(CapiExecutionListener, MultipleListenersAllReceiveEvents)
{
  // Two listeners; both should see every order event.
  State a, b;
  auto* la = flox_execution_listener_create(make_cb(a));
  auto* lb = flox_execution_listener_create(make_cb(b));

  auto* registry = flox_registry_create();
  uint32_t sym = flox_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* btr = flox_backtest_runner_create(registry, 0.0, 10000.0);
  flox_backtest_runner_add_execution_listener(btr, la);
  flox_backtest_runner_add_execution_listener(btr, lb);

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
    auto* s = static_cast<St*>(ud);
    flox_emit_market_buy(s->h, s->sym, 100000000LL);
  };

  FloxStrategyCallbacks scb{};
  scb.on_bar = on_bar;
  scb.user_data = &st;
  uint32_t syms[] = {sym};
  auto* strat = flox_strategy_create(1, syms, 1, registry, scb);
  st.h = strat;
  flox_backtest_runner_set_strategy(btr, strat);

  int64_t starts[2] = {1'000'000'000, 2'000'000'000};
  int64_t ends[2] = {1'999'999'999, 2'999'999'999};
  double opens[2] = {100.0, 101.0};
  double highs[2] = {101.0, 102.0};
  double lows[2] = {99.0, 100.5};
  double closes[2] = {100.5, 101.5};
  double volumes[2] = {10.0, 10.0};
  FloxBacktestStats stats{};
  flox_backtest_runner_run_bars(btr, starts, ends, opens, highs, lows, closes,
                                volumes, 2, "BTC", 0, 0, &stats);

  EXPECT_EQ(a.filled.load(), b.filled.load());
  EXPECT_GE(a.filled.load(), 1);

  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
  flox_execution_listener_destroy(la);
  flox_execution_listener_destroy(lb);
}

TEST(CapiExecutionListener, NullCallbacksAreNoOp)
{
  // Listener with all callbacks null shouldn't crash even when many
  // events fire.
  FloxExecutionListenerCallbacks cb{};  // all null
  auto* listener = flox_execution_listener_create(cb);
  ASSERT_NE(listener, nullptr);

  auto* registry = flox_registry_create();
  uint32_t sym = flox_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* btr = flox_backtest_runner_create(registry, 0.0, 10000.0);
  flox_backtest_runner_add_execution_listener(btr, listener);

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
    auto* s = static_cast<St*>(ud);
    flox_emit_market_buy(s->h, s->sym, 100000000LL);
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

  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
  flox_execution_listener_destroy(listener);
}
