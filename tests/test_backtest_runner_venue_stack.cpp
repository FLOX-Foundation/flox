/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// BacktestRunner driven by a VenueStack executor.
//
// setExecutor() routes order submission to a custom IOrderExecutor but
// keeps feeding market data to the built-in simulator, and the contract
// puts fill reporting on the custom executor — so a VenueStack attached
// that way produced a run with zero trades and a flat equity curve.
// setSimulatedExecutor() closes that gap: the runner feeds the venue
// executor and harvests its fills.

#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/venue_stack.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

using namespace flox;

namespace
{

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "binance";
  info.symbol = name;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

// Buys on the first trade, sells on the fifth: one closed round trip, so
// both fills and a realised PnL land in the result.
class RoundTripStrategy : public Strategy
{
 public:
  using Strategy::Strategy;
  size_t trades_seen{0};

  void set_symbol(SymbolId s) { _sym = s; }

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& /*ev*/) override
  {
    ++trades_seen;
    if (trades_seen == 1)
    {
      emitMarketBuy(_sym, Quantity::fromDouble(0.1));
    }
    else if (trades_seen == 5)
    {
      emitMarketSell(_sym, Quantity::fromDouble(0.1));
    }
  }

 private:
  SymbolId _sym{0};
};

std::filesystem::path writeTape(SymbolId sym, size_t n, const char* dir)
{
  auto td = std::filesystem::temp_directory_path() / dir;
  std::filesystem::remove_all(td);
  std::filesystem::create_directories(td);
  constexpr int64_t base_ns = 1'700'000'000'000'000'000;
  replay::WriterConfig cfg;
  cfg.output_dir = td;
  cfg.create_index = false;
  replay::BinaryLogWriter writer(cfg);
  for (size_t i = 0; i < n; ++i)
  {
    replay::TradeRecord r{};
    r.exchange_ts_ns = base_ns + static_cast<int64_t>(i) * 1'000'000'000;
    r.recv_ts_ns = r.exchange_ts_ns;
    r.price_raw = Price::fromDouble(100.0 + static_cast<double>(i) * 0.1).raw();
    r.qty_raw = Quantity::fromDouble(0.1).raw();
    r.symbol_id = sym;
    r.side = 1;
    writer.writeTrade(r);
  }
  writer.close();
  return td;
}

}  // namespace

TEST(BacktestRunnerVenueStack, VenueExecutorProducesFillsInTheResult)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTape(sym, 10, "flox_bt_venue_fills");

  RoundTripStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);

  auto stack = VenueStack::binance_um_futures(/*accountId=*/42, /*equity=*/10'000.0);

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&stack.executor(), &stack.clock());
  runner.runTape(tape);

  auto res = runner.result();
  auto stats = res.computeStats();

  // The whole point: a venue-stack run is no longer silently empty.
  EXPECT_GT(stack.executor().fills().size(), 0u)
      << "the venue executor must receive market data and fill orders";
  EXPECT_GT(stats.totalTrades, 0u)
      << "fills from the venue executor must reach BacktestResult";

  std::filesystem::remove_all(tape);
}

TEST(BacktestRunnerVenueStack, VenueClockAdvancesWithTheReplay)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTape(sym, 10, "flox_bt_venue_clock");

  RoundTripStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);

  auto stack = VenueStack::binance_um_futures(42, 10'000.0);
  EXPECT_EQ(stack.clock().nowNs().raw(), 0) << "a fresh stack clock starts at zero";

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&stack.executor(), &stack.clock());
  runner.runTape(tape);

  // Without this the venue executor would model ack latency and queue
  // timing against a frozen clock.
  EXPECT_GT(stack.clock().nowNs().raw(), 0)
      << "the runner must advance the venue clock in lockstep";

  std::filesystem::remove_all(tape);
}

TEST(BacktestRunnerVenueStack, RevertingToTheBuiltInExecutorStillWorks)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTape(sym, 10, "flox_bt_venue_revert");

  RoundTripStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);

  auto stack = VenueStack::binance_um_futures(42, 10'000.0);

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&stack.executor(), &stack.clock());
  runner.setSimulatedExecutor(nullptr, nullptr);
  runner.runTape(tape);

  EXPECT_EQ(runner.simulatedExecutorOverride(), nullptr);
  EXPECT_EQ(stack.executor().fills().size(), 0u)
      << "a detached venue executor must not see the run";
  EXPECT_GT(runner.result().computeStats().totalTrades, 0u)
      << "the built-in executor must still produce the run's fills";

  std::filesystem::remove_all(tape);
}
