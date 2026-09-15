/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Regression tests for the tracker/hook ownership race: the live engine's
// bus consumer threads dereference the last-installed PnL tracker (and the
// other optional signal hooks) inside RunnerSignalHandler::onSignal, while
// flox_*_destroy() may run concurrently on a different thread the instant
// the hook has been detached with set(NULL). Setting the pointer to NULL
// only stops *future* dispatches from picking it up -- a dispatch already
// past the load, mid-callback with the old pointer, is unaffected by it.
//
// TrackerLifetimeStress reproduces this directly: one thread keeps
// publishing trades (which the strategy turns into signals, so every trade
// dereferences the currently-installed PnL tracker) while another thread
// repeatedly attaches, detaches and destroys trackers. Before the ownership
// fix, running this under AddressSanitizer reliably reports a
// heap-use-after-free within the loop below; after the fix, the object a
// dispatch thread is using stays alive for as long as that dispatch needs
// it, however destroy() is sequenced against set(NULL).
//
// TrackerLifetimeBlockingCallback pins down the specific scenario named in
// the task: a callback that is *actively executing* when destroy() runs on
// another thread must be allowed to run to completion safely.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace
{

struct LifetimeState
{
  FloxStrategyHandle strategy{nullptr};
  uint32_t symbol_id{0};
  std::atomic<int> on_signal_calls{0};
  std::atomic<int> pnl_calls{0};
};

void on_trade_cb(void* ud, const FloxSymbolContext*, const FloxTradeData*)
{
  auto* st = static_cast<LifetimeState*>(ud);
  flox_emit_market_buy(st->strategy, st->symbol_id, flox_quantity_from_double(1.0));
}

void on_signal_cb(void* ud, const FloxSignal*)
{
  static_cast<LifetimeState*>(ud)->on_signal_calls.fetch_add(1, std::memory_order_relaxed);
}

void counting_pnl_cb(void* ud, const FloxSignal*)
{
  static_cast<LifetimeState*>(ud)->pnl_calls.fetch_add(1, std::memory_order_relaxed);
}

struct EngineCtx
{
  FloxRegistryHandle registry{nullptr};
  FloxStrategyHandle strategy{nullptr};
  FloxLiveEngineHandle engine{nullptr};
  uint32_t symbol_id{0};

  ~EngineCtx()
  {
    if (engine)
    {
      flox_live_engine_stop(engine);
      flox_live_engine_destroy(engine);
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

void make_engine(EngineCtx& ctx, LifetimeState& state)
{
  ctx.registry = flox_registry_create();
  ASSERT_NE(ctx.registry, nullptr);
  ctx.symbol_id = flox_registry_add_symbol(ctx.registry, "test", "BTC", 0.01);
  state.symbol_id = ctx.symbol_id;

  FloxStrategyCallbacks cb{};
  cb.on_trade = on_trade_cb;
  cb.user_data = &state;
  uint32_t syms[] = {ctx.symbol_id};
  ctx.strategy = flox_strategy_create(/*id=*/1, syms, 1, ctx.registry, cb);
  ASSERT_NE(ctx.strategy, nullptr);
  state.strategy = ctx.strategy;

  ctx.engine = flox_live_engine_create(ctx.registry);
  ASSERT_NE(ctx.engine, nullptr);
  flox_live_engine_add_strategy(ctx.engine, ctx.strategy, on_signal_cb, &state);
  flox_live_engine_start(ctx.engine);
}

}  // namespace

// One thread continuously publishes trades, which the strategy turns into
// signals dispatched from the trade bus's own consumer thread -- every
// dispatch dereferences whatever PnL tracker is currently installed.
// Concurrently, the main thread repeatedly installs a tracker, detaches it
// with set(NULL), and destroys it. Run under ASan/TSan (see
// verify-docs-current's sanitizer job), this pins down the exact defect
// this test guards against: destroy() used to free the tracker the instant
// it was called, with no regard for a dispatch thread that had already
// loaded the old pointer.
TEST(CapiTrackerLifetime, StressDestroyDuringDispatch)
{
  LifetimeState state;
  EngineCtx ctx;
  make_engine(ctx, state);

  std::atomic<bool> stop{false};
  std::thread publisher(
      [&]
      {
        int64_t ts = 1'000'000'000LL;
        while (!stop.load(std::memory_order_relaxed))
        {
          flox_live_engine_publish_trade(ctx.engine, state.symbol_id, 100.0, 1.0, 1, ts++);
        }
      });

  // No throttling on either side: the point is to hammer the window
  // between "the dispatch thread has loaded the tracker pointer" and "the
  // admin thread has freed it" as hard as possible. Several churn threads
  // run concurrently so the single trade-bus dispatch thread has many
  // chances per second to race a concurrent destroy().
  constexpr int kChurnThreads = 4;
  constexpr int kChurnIterationsPerThread = 20000;
  std::vector<std::thread> churners;
  for (int t = 0; t < kChurnThreads; ++t)
  {
    churners.emplace_back(
        [&]
        {
          for (int i = 0; i < kChurnIterationsPerThread; ++i)
          {
            FloxPnLTrackerCallbacks cb{};
            cb.on_signal = counting_pnl_cb;
            cb.user_data = &state;
            FloxPnLTrackerHandle tracker = flox_pnl_tracker_create(cb);

            flox_live_engine_set_pnl_tracker(ctx.engine, tracker);
            flox_live_engine_set_pnl_tracker(ctx.engine, nullptr);
            flox_pnl_tracker_destroy(tracker);
          }
        });
  }
  for (auto& th : churners)
  {
    th.join();
  }

  stop.store(true, std::memory_order_relaxed);
  publisher.join();

  // Reaching here without a sanitizer report is the point of the test; the
  // counters just confirm the dispatch loop was actually doing work.
  EXPECT_GT(state.on_signal_calls.load(), 0);
}

// Deterministic version of the same guarantee: a callback that is already
// running when destroy() is called on another thread must be allowed to
// finish untouched. The old pointer stays valid for exactly as long as the
// in-flight dispatch needs it, no matter what set(NULL) + destroy() do
// concurrently.
TEST(CapiTrackerLifetime, RevokeDuringInFlightCallbackIsSafe)
{
  LifetimeState state;
  EngineCtx ctx;
  make_engine(ctx, state);

  std::atomic<bool> entered{false};
  std::atomic<bool> may_finish{false};
  std::atomic<int> completed{0};

  struct BlockingCtx
  {
    std::atomic<bool>* entered;
    std::atomic<bool>* may_finish;
    std::atomic<int>* completed;
  } blocking_ctx{&entered, &may_finish, &completed};

  auto blocking_cb = [](void* ud, const FloxSignal*)
  {
    auto* b = static_cast<BlockingCtx*>(ud);
    b->entered->store(true, std::memory_order_release);
    while (!b->may_finish->load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    b->completed->fetch_add(1, std::memory_order_release);
  };

  FloxPnLTrackerCallbacks cb{};
  cb.on_signal = blocking_cb;
  cb.user_data = &blocking_ctx;
  FloxPnLTrackerHandle tracker = flox_pnl_tracker_create(cb);
  ASSERT_NE(tracker, nullptr);
  flox_live_engine_set_pnl_tracker(ctx.engine, tracker);

  flox_live_engine_publish_trade(ctx.engine, state.symbol_id, 100.0, 1.0, 1, 1'000'000'000LL);

  // Wait for the dispatch thread to actually be inside the callback with
  // the tracker in hand.
  for (int i = 0; i < 5000 && !entered.load(std::memory_order_acquire); ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(entered.load(std::memory_order_acquire))
      << "dispatch thread never entered the PnL callback";

  // The documented detach sequence, run while the callback above is known
  // to be blocked mid-flight with the old pointer.
  flox_live_engine_set_pnl_tracker(ctx.engine, nullptr);
  flox_pnl_tracker_destroy(tracker);

  may_finish.store(true, std::memory_order_release);

  for (int i = 0; i < 5000 && completed.load(std::memory_order_acquire) == 0; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(completed.load(std::memory_order_acquire), 1)
      << "in-flight callback did not complete after being revoked underneath it";
}
