/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Regression test for `BacktestRunner::runTape(path)` — the canonical
// recorded-tape → backtest path. Without `runTape` the only way to
// drive a strategy off a `.floxlog` was a sidecar (e.g. bars.npz) since
// `replay_tape` is callback-based and doesn't feed the runner.
//
// Test writes a tiny tape via `BinaryLogWriter`, then drives a
// counter strategy off it through `runTape` and asserts the
// strategy's `onSymbolTrade` fires the expected number of times.

#include "flox/backtest/backtest_runner.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <vector>

using namespace flox;

namespace
{

class TradeCounter : public Strategy
{
 public:
  using Strategy::Strategy;
  size_t trades_seen{0};

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& /*ev*/) override
  {
    ++trades_seen;
  }
};

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

}  // namespace

TEST(BacktestRunTape, RoundTripsRecordedSession)
{
  auto td = std::filesystem::temp_directory_path() / "flox_run_tape_test";
  std::filesystem::remove_all(td);
  std::filesystem::create_directories(td);

  // Write a tiny tape: 50 trades on one symbol.
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  constexpr int64_t base_ns = 1'700'000'000'000'000'000;
  constexpr size_t kTradeCount = 50;
  {
    replay::WriterConfig cfg;
    cfg.output_dir = td;
    cfg.create_index = false;
    replay::BinaryLogWriter writer(cfg);
    for (size_t i = 0; i < kTradeCount; ++i)
    {
      replay::TradeRecord r{};
      r.exchange_ts_ns = base_ns + static_cast<int64_t>(i) * 1'000'000'000;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = Price::fromDouble(100.0 + i * 0.1).raw();
      r.qty_raw = Quantity::fromDouble(0.1).raw();
      r.symbol_id = sym;
      r.side = 1;
      r.trade_id = i + 1;
      writer.writeTrade(r);
    }
    writer.close();
  }

  TradeCounter strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runTape(td);

  EXPECT_EQ(strat.trades_seen, kTradeCount);

  std::filesystem::remove_all(td);
}

TEST(BacktestRunTape, ThrowsOnMissingDirectory)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  TradeCounter strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner;
  runner.setStrategy(&strat);

  // The factory either returns nullptr (caught + thrown) or throws.
  // Either way the call must not silently produce zero-trade stats.
  auto missing = std::filesystem::temp_directory_path() / "flox_run_tape_does_not_exist";
  std::filesystem::remove_all(missing);
  EXPECT_ANY_THROW(runner.runTape(missing));
}
