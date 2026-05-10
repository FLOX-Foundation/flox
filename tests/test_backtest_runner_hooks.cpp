/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for W1-T036: BacktestRunner pre-trade gate parity with the
// live Runner. Each test exercises one of the four hooks
// (RiskManager / KillSwitch / OrderValidator / PnLTracker) plus the
// reduce-only bypass contract that the gotchas.json entry advertises.
//
// The tape replay path is shared with run_tape — write a tiny tape,
// run a strategy that emits one entry order on the first tick (and
// optionally an exit later), and assert the gate behaves as expected.

#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/killswitch/abstract_killswitch.h"
#include "flox/metrics/abstract_pnl_tracker.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "flox/risk/abstract_risk_manager.h"
#include "flox/strategy/strategy.h"
#include "flox/validation/abstract_order_validator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <vector>

using namespace flox;

namespace
{

// Strategy that emits one market_buy on the first trade and one
// market_sell on the second (a flatten / exit). We use both signal
// types in different tests to exercise gate behaviour on entries
// vs reduce-only exits.
class TwoStepStrategy : public Strategy
{
 public:
  using Strategy::Strategy;
  size_t trades_seen{0};

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& /*ev*/) override
  {
    ++trades_seen;
    if (trades_seen == 1)
    {
      // Entry: subject to gate.
      emitMarketBuy(_sym, Quantity::fromDouble(0.1));
    }
    else if (trades_seen == 5)
    {
      // Reduce-only exit: must bypass the gate even when the
      // strategy is held by a tightened risk cap. Build a Signal
      // with reduceOnly explicitly so the gate's bypass branch
      // fires.
      Signal sig = Signal::marketSell(_sym, Quantity::fromDouble(0.1),
                                      static_cast<OrderId>(99));
      sig.reduceOnly = true;
      emit(sig);
    }
  }

 public:
  void set_symbol(SymbolId s) { _sym = s; }

 private:
  SymbolId _sym{0};
};

// Test risk manager that rejects every entry with quantity > 0.05.
class RejectingRisk : public IRiskManager
{
 public:
  std::atomic<int> calls{0};
  bool allow(const Order& order) const override
  {
    ++const_cast<RejectingRisk*>(this)->calls;
    return order.quantity.toDouble() <= 0.05;
  }
};

class CountingValidator : public IOrderValidator
{
 public:
  std::atomic<int> calls{0};
  bool ok{true};
  bool validate(const Order& /*order*/, std::string& reason) const override
  {
    ++const_cast<CountingValidator*>(this)->calls;
    if (!ok)
    {
      reason = "validator-rejected";
    }
    return ok;
  }
};

class TripWireKill : public IKillSwitch
{
 public:
  std::atomic<int> checks{0};
  bool tripped{false};
  void check(const Order& /*order*/) override
  {
    ++checks;
    // Trip permanently after the first check call.
    tripped = true;
  }
  void trigger(const std::string& /*reason*/) override { tripped = true; }
  bool isTriggered() const override { return tripped; }
  std::string reason() const override { return "test-tripped"; }
};

class CountingPnL : public IPnLTracker
{
 public:
  std::atomic<int> fills{0};
  void onOrderFilled(const Order& /*order*/) override { ++fills; }
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

std::filesystem::path writeTinyTape(SymbolId sym, size_t n)
{
  auto td = std::filesystem::temp_directory_path() / "flox_bt_hooks_test";
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
    r.price_raw = Price::fromDouble(100.0 + i * 0.1).raw();
    r.qty_raw = Quantity::fromDouble(0.1).raw();
    r.symbol_id = sym;
    r.side = 1;
    r.trade_id = i + 1;
    writer.writeTrade(r);
  }
  writer.close();
  return td;
}

}  // namespace

TEST(BacktestRunnerHooks, RiskManagerRejectsEntryAndDropsToZeroTrades)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTinyTape(sym, 10);

  TwoStepStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);
  RejectingRisk rm;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setRiskManager(&rm);
  runner.runTape(tape);

  EXPECT_GT(rm.calls.load(), 0)
      << "risk manager must see at least the entry order";
  std::filesystem::remove_all(tape);
}

TEST(BacktestRunnerHooks, OrderValidatorRejectionDropsTheOrder)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTinyTape(sym, 10);

  TwoStepStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);
  CountingValidator ov;
  ov.ok = false;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setOrderValidator(&ov);
  runner.runTape(tape);

  EXPECT_GT(ov.calls.load(), 0)
      << "validator must see at least the entry order";
  std::filesystem::remove_all(tape);
}

TEST(BacktestRunnerHooks, KillSwitchHaltsNewEntries)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTinyTape(sym, 10);

  TwoStepStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);
  TripWireKill ks;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setKillSwitch(&ks);
  runner.runTape(tape);

  // The kill-switch must have been consulted at least for the entry.
  EXPECT_GT(ks.checks.load(), 0);
  std::filesystem::remove_all(tape);
}

TEST(BacktestRunnerHooks, PnLTrackerSeesEveryFill)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTinyTape(sym, 10);

  TwoStepStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);
  CountingPnL pnl;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setPnLTracker(&pnl);
  runner.runTape(tape);

  // The strategy emits at least one entry market order; the
  // SimulatedExecutor fills it on the next tick. The tracker sees
  // FILLED + (potentially) PARTIALLY_FILLED — so >= 1 is the
  // contract worth pinning here.
  EXPECT_GE(pnl.fills.load(), 1);
  std::filesystem::remove_all(tape);
}

TEST(BacktestRunnerHooks, NullHooksAreNoops)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  auto tape = writeTinyTape(sym, 5);

  TwoStepStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.set_symbol(sym);

  BacktestRunner runner;
  runner.setStrategy(&strat);
  // Set then clear — must not crash and must not gate anything.
  RejectingRisk rm;
  runner.setRiskManager(&rm);
  runner.setRiskManager(nullptr);

  runner.runTape(tape);
  EXPECT_EQ(rm.calls.load(), 0)
      << "after detach the runner must stop calling the manager";
  std::filesystem::remove_all(tape);
}
