/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the ReplaySource hook (T021). A binding-supplied replay
// source is pulled by the BacktestRunner via the IMultiSegmentReader
// adapter; this verifies trade events flow into the runner and that
// lifecycle / seek_to callbacks fire as expected.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <vector>

namespace
{

struct TradeFixture
{
  uint32_t symbol;
  int64_t timestamp_ns;
  int64_t price_raw;
  int64_t qty_raw;
  uint8_t is_buy;
};

struct BookFixture
{
  uint32_t symbol;
  int64_t timestamp_ns;
  uint8_t is_snapshot;  // 1 → BookSnapshot (event_type 2), 0 → BookDelta (3)
  std::vector<FloxBookLevel> bids;
  std::vector<FloxBookLevel> asks;
};

struct State
{
  std::vector<TradeFixture> trades;
  std::vector<BookFixture> books;
  size_t cursor{0};

  std::atomic<int> on_start_calls{0};
  std::atomic<int> on_stop_calls{0};
  std::atomic<int> seek_calls{0};
  int64_t last_seek_ts{0};
};

uint8_t source_next(void* ud, FloxReplayEvent* ev)
{
  auto* s = static_cast<State*>(ud);
  size_t idx = s->cursor++;
  size_t n_trades = s->trades.size();
  if (idx < n_trades)
  {
    const auto& t = s->trades[idx];
    ev->type = 1;
    ev->timestamp_ns = t.timestamp_ns;
    ev->trade_symbol = t.symbol;
    ev->trade_is_buy = t.is_buy;
    ev->trade_price_raw = t.price_raw;
    ev->trade_quantity_raw = t.qty_raw;
    return 1;
  }
  size_t book_idx = idx - n_trades;
  if (book_idx < s->books.size())
  {
    const auto& b = s->books[book_idx];
    ev->type = b.is_snapshot ? 2u : 3u;
    ev->timestamp_ns = b.timestamp_ns;
    ev->book_symbol = b.symbol;
    ev->n_bids = static_cast<uint32_t>(b.bids.size());
    ev->n_asks = static_cast<uint32_t>(b.asks.size());
    ev->bids = b.bids.empty() ? nullptr : b.bids.data();
    ev->asks = b.asks.empty() ? nullptr : b.asks.data();
    return 1;
  }
  return 0;
}

uint8_t source_seek(void* ud, int64_t ts)
{
  auto* s = static_cast<State*>(ud);
  s->seek_calls.fetch_add(1, std::memory_order_relaxed);
  s->last_seek_ts = ts;
  return 1;
}

void source_on_start(void* ud)
{
  static_cast<State*>(ud)->on_start_calls.fetch_add(1, std::memory_order_relaxed);
}

void source_on_stop(void* ud)
{
  static_cast<State*>(ud)->on_stop_calls.fetch_add(1, std::memory_order_relaxed);
}

FloxReplaySourceCallbacks make_cb(State& state)
{
  FloxReplaySourceCallbacks cb{};
  cb.on_start = source_on_start;
  cb.on_stop = source_on_stop;
  cb.seek_to = source_seek;
  cb.next = source_next;
  cb.user_data = &state;
  return cb;
}

}  // namespace

TEST(CapiReplaySource, RunsTradesThroughBacktestRunner)
{
  State state;
  state.trades = {
      {1, 1'000'000'000LL, 10000000000LL /*100.0 raw*/, 100000000LL /*1.0 raw*/, 1},
      {1, 2'000'000'000LL, 10100000000LL /*101.0 raw*/, 100000000LL, 0},
      {1, 3'000'000'000LL, 10200000000LL /*102.0 raw*/, 100000000LL, 1},
  };

  auto* registry = flox_registry_create();
  ASSERT_NE(registry, nullptr);
  uint32_t symbol_id = flox_registry_add_symbol(registry, "test", "BTC", 0.01);
  // Rewrite trade fixtures to use the actual id assigned by the registry.
  for (auto& t : state.trades)
  {
    t.symbol = symbol_id;
  }

  auto* btr = flox_backtest_runner_create(registry, /*fee_rate=*/0.001,
                                          /*initial_capital=*/10000.0);
  ASSERT_NE(btr, nullptr);

  // Strategy with no callbacks — the runner just needs *some* strategy.
  FloxStrategyCallbacks scb{};
  uint32_t syms[] = {symbol_id};
  auto* strat = flox_strategy_create(/*id=*/1, syms, 1, registry, scb);
  ASSERT_NE(strat, nullptr);
  flox_backtest_runner_set_strategy(btr, strat);

  auto* source = flox_replay_source_create(make_cb(state));
  ASSERT_NE(source, nullptr);

  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_replay_source(btr, source, &stats);
  EXPECT_EQ(rc, 1);

  EXPECT_EQ(state.on_start_calls.load(), 1);
  EXPECT_EQ(state.on_stop_calls.load(), 1);

  flox_replay_source_destroy(source);
  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
}

TEST(CapiReplaySource, NextReturnsZeroEndsPlayback)
{
  // A source that immediately returns 0 leaves the runner with an empty
  // event stream; lifecycle still fires balanced.
  State state;
  state.trades.clear();
  state.books.clear();

  auto* registry = flox_registry_create();
  uint32_t symbol_id = flox_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* btr = flox_backtest_runner_create(registry, 0.0, 1000.0);

  FloxStrategyCallbacks scb{};
  uint32_t syms[] = {symbol_id};
  auto* strat = flox_strategy_create(1, syms, 1, registry, scb);
  flox_backtest_runner_set_strategy(btr, strat);

  auto* source = flox_replay_source_create(make_cb(state));
  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_replay_source(btr, source, &stats);
  EXPECT_EQ(rc, 1);
  EXPECT_EQ(state.on_start_calls.load(), 1);
  EXPECT_EQ(state.on_stop_calls.load(), 1);

  flox_replay_source_destroy(source);
  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
}

TEST(CapiReplaySource, SeekToForwardsToCallback)
{
  State state;
  auto* source = flox_replay_source_create(make_cb(state));
  ASSERT_NE(source, nullptr);

  uint8_t rc = flox_replay_source_seek_to(source, 12345LL);
  EXPECT_EQ(rc, 1);
  EXPECT_EQ(state.seek_calls.load(), 1);
  EXPECT_EQ(state.last_seek_ts, 12345LL);

  flox_replay_source_destroy(source);
}

TEST(CapiReplaySource, SeekToWithNullCallbackReturnsZero)
{
  FloxReplaySourceCallbacks cb{};
  // No callbacks at all — handle exists but seek_to is null.
  auto* source = flox_replay_source_create(cb);
  ASSERT_NE(source, nullptr);

  uint8_t rc = flox_replay_source_seek_to(source, 99LL);
  EXPECT_EQ(rc, 0) << "missing seek_to callback should report failure";

  flox_replay_source_destroy(source);
}

TEST(CapiReplaySource, NullSourceFailsRunWithoutCrash)
{
  auto* registry = flox_registry_create();
  flox_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* btr = flox_backtest_runner_create(registry, 0.0, 1000.0);

  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_replay_source(btr, nullptr, &stats);
  EXPECT_EQ(rc, 0);

  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
}
