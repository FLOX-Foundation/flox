/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The bar path of the C API against the bar path of the engine.
//
// BacktestRunner::runBars walks every bar open -> low -> high -> close, holds
// whatever the bar callback submits, and releases it at the next bar's open.
// A binding consumer driving SimulatedExecutor by hand has none of that: the C
// API offers `flox_simulated_executor_on_bar(handle, symbol, close)` and
// nothing else, so it moves the market straight to the close, never sees an
// intrabar extreme, and matches a callback order at a price that exists only
// because the bar has already happened.
//
// Each case here drives the C API by hand over the same bars the runner is
// given and requires the same fills out of both. The functions the hand-drive
// needs do not exist yet, so they are resolved by name at run time rather than
// called directly: that keeps the file compiling and linking against today's
// header, and makes the absence a named test failure instead of a build break.
//
// needs: void flox_simulated_executor_on_bar_ohlc(FloxSimulatedExecutorHandle executor,
//                                                 uint32_t symbol, double open_price,
//                                                 double high_price, double low_price,
//                                                 double close_price);
// needs: void flox_simulated_executor_begin_bar_callback_window(FloxSimulatedExecutorHandle executor);
// needs: void flox_simulated_executor_end_bar_callback_window(FloxSimulatedExecutorHandle executor);
// needs: void flox_simulated_executor_reset(FloxSimulatedExecutorHandle executor);
// needs: uint8_t FloxBar::close_reason, carrying flox::Bar::reason through batch aggregation.

// This is the one capi test that also links the engine headers, so the
// reference run and the run under test sit in one process. The executable then
// holds two copies of the engine -- the static `flox` it links and the one
// `flox_capi` embeds -- which is safe here because neither path reads engine
// global state: bar times are passed as raw nanoseconds on both sides and the
// timebase mapping is never consulted. Keep it that way; see the note on
// test_capi_logger in tests/CMakeLists.txt for what goes wrong when a test
// straddles both copies of a global.

#include "flox/capi/flox_capi.h"

#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/symbol_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace flox;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;
constexpr uint32_t kSym = 1;

// The tape both paths are given.
//
// Bar 0 closes at 105, and bar 1 opens at 106 -- a price that appears nowhere
// in bar 0, so a fill at the open is distinguishable from a fill at anything
// the bar-0 callback was shown. Bar 1 runs its high up to 109.5, above the
// resting sell at 109 and above its own close of 107, so a fill there can only
// come from the intrabar extreme.
struct BarRow
{
  double open;
  double high;
  double low;
  double close;
  int64_t endNs;
};

const std::vector<BarRow>& tape()
{
  static const std::vector<BarRow> rows = {
      {100.0, 110.0, 90.0, 105.0, kMinuteNs},
      {106.0, 109.5, 104.0, 107.0, 2 * kMinuteNs},
      {103.0, 104.0, 102.0, 103.5, 3 * kMinuteNs},
  };
  return rows;
}

// What both paths have to produce, as a comparable record. Order id, side,
// price and quantity only: the two paths run separate clocks, and the fill
// timestamp is asserted on its own where it matters.
struct FillRow
{
  uint64_t orderId;
  uint8_t side;  // 0 = buy, 1 = sell
  double price;
  double quantity;

  bool operator==(const FillRow& other) const
  {
    return orderId == other.orderId && side == other.side &&
           price == other.price && quantity == other.quantity;
  }
};

std::string describe(const std::vector<FillRow>& fills)
{
  std::string out;
  for (const auto& f : fills)
  {
    out += "\n  id=" + std::to_string(f.orderId) +
           (f.side == 0 ? " buy " : " sell ") + std::to_string(f.price) +
           " x " + std::to_string(f.quantity);
  }
  return out.empty() ? std::string("\n  (none)") : out;
}

SymbolId addSymbol(SymbolRegistry& reg, const std::string& spelling)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

BarEvent makeBar(SymbolId sym, const BarRow& row)
{
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = Price::fromDouble(row.open);
  ev.bar.high = Price::fromDouble(row.high);
  ev.bar.low = Price::fromDouble(row.low);
  ev.bar.close = Price::fromDouble(row.close);
  ev.bar.volume = Volume::fromDouble(1000.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{row.endNs - kMinuteNs}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{row.endNs}};
  ev.bar.reason = BarCloseReason::Threshold;
  return ev;
}

// The two orders both paths send, from the callback of bar 0:
//   id 1 -- market buy, which can only fill at the next bar's open;
//   id 2 -- limit sell at 109, which rests through the release at 106 and can
//           only fill against bar 1's high of 109.5.
Order marketBuy(SymbolId sym)
{
  Order o{};
  o.id = 1;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(1.0);
  return o;
}

Order limitSell(SymbolId sym)
{
  Order o{};
  o.id = 2;
  o.symbol = sym;
  o.side = Side::SELL;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(109.0);
  o.quantity = Quantity::fromDouble(1.0);
  return o;
}

// Sends both orders from the bar-0 callback, so the runner holds them exactly
// as it holds a strategy's own orders.
class BarZeroSubmitter : public IMarketDataSubscriber
{
 public:
  explicit BarZeroSubmitter(SimulatedExecutor& exec) : _exec(exec) {}

  SubscriberId id() const override { return 0xB1B1u; }

  void onBar(const BarEvent& ev) override
  {
    if (_sent)
    {
      return;
    }
    _sent = true;
    _exec.submitOrder(marketBuy(ev.symbol));
    _exec.submitOrder(limitSell(ev.symbol));
  }

  void rearm() { _sent = false; }

 private:
  SimulatedExecutor& _exec;
  bool _sent{false};
};

// Written as a concept rather than a bare requires-expression so the check is
// a dependent one: a missing member is then a false constraint instead of a
// compile error, which is the whole point of asking.
template <typename T>
concept HasCloseReason = requires(T bar) { bar.close_reason; };

// Aggregates a five-trade minute tape and checks the reason every returned bar
// carries. Kept in a template so the body is only instantiated -- only parsed
// against the real struct -- once the field is there.
template <typename BarT>
void expectAggregatedCloseReason()
{
  if constexpr (HasCloseReason<BarT>)
  {
    const int64_t ts[] = {0, 30'000'000'000LL, 61'000'000'000LL, 91'000'000'000LL,
                          121'000'000'000LL};
    const double px[] = {100.0, 101.0, 102.0, 103.0, 104.0};
    const double qty[] = {1.0, 1.0, 1.0, 1.0, 1.0};
    const uint8_t isBuy[] = {1, 1, 1, 1, 1};

    BarT bars[8]{};
    const uint32_t n = flox_aggregate_time_bars(ts, px, qty, isBuy, 5, 60.0, bars, 8);
    ASSERT_GE(n, 1u);
    for (uint32_t i = 0; i < n; ++i)
    {
      EXPECT_EQ(bars[i].close_reason, static_cast<uint8_t>(BarCloseReason::Threshold))
          << "bar " << i << " reports a close reason the aggregator never set";
    }
  }
}

std::vector<FillRow> toRows(const std::vector<Fill>& fills)
{
  std::vector<FillRow> rows;
  rows.reserve(fills.size());
  for (const auto& f : fills)
  {
    rows.push_back({f.orderId, static_cast<uint8_t>(f.side == Side::BUY ? 0 : 1),
                    f.price.toDouble(), f.quantity.toDouble()});
  }
  return rows;
}

std::vector<FillRow> toRows(const std::vector<FloxFill>& fills)
{
  std::vector<FillRow> rows;
  rows.reserve(fills.size());
  for (const auto& f : fills)
  {
    rows.push_back({f.order_id, f.side, Price::fromRaw(f.price_raw).toDouble(),
                    Quantity::fromRaw(f.quantity_raw).toDouble()});
  }
  return rows;
}

// The reference: the fills BacktestRunner::runBars produces over `tape()`.
std::vector<FillRow> runnerFills()
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  BacktestRunner runner;
  BarZeroSubmitter sub(runner.executor());
  runner.addMarketDataSubscriber(&sub);

  std::vector<BarEvent> bars;
  for (const auto& row : tape())
  {
    bars.push_back(makeBar(sym, row));
  }
  runner.runBars(bars);

  return toRows(runner.executor().fills());
}

// ── the C entry points the hand-drive needs ────────────────────────────────

void* symbolNamed(const char* name)
{
#if defined(_WIN32)
  HMODULE self = GetModuleHandleA(nullptr);
  return self ? reinterpret_cast<void*>(GetProcAddress(self, name)) : nullptr;
#else
  return dlsym(RTLD_DEFAULT, name);
#endif
}

using OnBarOhlcFn = void (*)(FloxSimulatedExecutorHandle, uint32_t, double, double, double,
                             double);
using WindowFn = void (*)(FloxSimulatedExecutorHandle);
using ResetFn = void (*)(FloxSimulatedExecutorHandle);

struct BarApi
{
  OnBarOhlcFn onBarOhlc{nullptr};
  WindowFn beginWindow{nullptr};
  WindowFn endWindow{nullptr};
  ResetFn reset{nullptr};

  bool complete() const
  {
    return onBarOhlc && beginWindow && endWindow && reset;
  }
};

BarApi loadBarApi()
{
  BarApi api;
  api.onBarOhlc = reinterpret_cast<OnBarOhlcFn>(
      symbolNamed("flox_simulated_executor_on_bar_ohlc"));
  api.beginWindow = reinterpret_cast<WindowFn>(
      symbolNamed("flox_simulated_executor_begin_bar_callback_window"));
  api.endWindow = reinterpret_cast<WindowFn>(
      symbolNamed("flox_simulated_executor_end_bar_callback_window"));
  api.reset = reinterpret_cast<ResetFn>(symbolNamed("flox_simulated_executor_reset"));
  return api;
}

std::vector<FillRow> readFills(FloxSimulatedExecutorHandle exec)
{
  const uint32_t n = flox_simulated_executor_fill_count(exec);
  std::vector<FloxFill> raw(n);
  if (n > 0)
  {
    const uint32_t got = flox_simulated_executor_get_fills(exec, raw.data(), n);
    raw.resize(got);
  }
  return toRows(raw);
}

// The hand-drive: exactly what runBars does per bar, spelled in C.
void driveTape(const BarApi& api, FloxSimulatedExecutorHandle exec)
{
  bool sent = false;
  for (const auto& row : tape())
  {
    flox_simulated_executor_advance_clock(exec, row.endNs);
    api.onBarOhlc(exec, kSym, row.open, row.high, row.low, row.close);

    api.beginWindow(exec);
    if (!sent)
    {
      sent = true;
      flox_simulated_executor_submit_order(exec, /*id=*/1, /*side=buy=*/0, 0.0, 1.0,
                                           static_cast<uint8_t>(OrderType::MARKET), kSym);
      flox_simulated_executor_submit_order(exec, /*id=*/2, /*side=sell=*/1, 109.0, 1.0,
                                           static_cast<uint8_t>(OrderType::LIMIT), kSym);
    }
    api.endWindow(exec);
  }
}

}  // namespace

// Green control, and the reference the C cases are written against: the C++
// executor already has every call the hand-drive needs, and driving it by hand
// over the tape reproduces the runner exactly. So the expected fills below are
// the engine's own answer, not a number picked to match an implementation --
// and a C case failing means the C path diverged, not that the tape is wrong.
TEST(CapiExecutorBarOhlc, TheCxxHandDriveAlreadyMatchesTheRunner)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  bool sent = false;
  for (const auto& row : tape())
  {
    clock.advanceTo(UnixNanos::fromRaw(row.endNs));
    exec.onBar(kSym, Price::fromDouble(row.open), Price::fromDouble(row.high),
               Price::fromDouble(row.low), Price::fromDouble(row.close));

    SimulatedExecutor::BarCallbackScope window(exec);
    if (!sent)
    {
      sent = true;
      exec.submitOrder(marketBuy(kSym));
      exec.submitOrder(limitSell(kSym));
    }
  }

  const std::vector<FillRow> actual = toRows(exec.fills());
  const std::vector<FillRow> expected = runnerFills();
  ASSERT_EQ(actual, expected) << "runner:" << describe(expected)
                              << "\nby hand:" << describe(actual);

  // Spelled out, so the numbers the C cases assert are visible here too.
  ASSERT_EQ(actual.size(), 2u) << describe(actual);
  EXPECT_DOUBLE_EQ(actual[0].price, 106.0) << "the market buy did not fill at the open";
  EXPECT_DOUBLE_EQ(actual[1].price, 109.0) << "the resting sell did not fill at its price";
}

// The four entry points, named so a missing one says which. Everything below
// is a consequence of these existing.
TEST(CapiExecutorBarOhlc, TheBarPathIsReachableFromC)
{
  const BarApi api = loadBarApi();

  // Green control: the close-only form is there, which is the whole of the
  // bar path a binding can reach today.
  EXPECT_NE(symbolNamed("flox_simulated_executor_on_bar"), nullptr);

  EXPECT_NE(api.onBarOhlc, nullptr)
      << "needs void flox_simulated_executor_on_bar_ohlc(FloxSimulatedExecutorHandle, "
         "uint32_t symbol, double open_price, double high_price, double low_price, "
         "double close_price) -- a binding driving the executor by hand has no way "
         "to feed a bar's open or its intrabar extremes";
  EXPECT_NE(api.beginWindow, nullptr)
      << "needs void flox_simulated_executor_begin_bar_callback_window("
         "FloxSimulatedExecutorHandle)";
  EXPECT_NE(api.endWindow, nullptr)
      << "needs void flox_simulated_executor_end_bar_callback_window("
         "FloxSimulatedExecutorHandle)";
  EXPECT_NE(api.reset, nullptr)
      << "needs void flox_simulated_executor_reset(FloxSimulatedExecutorHandle) -- "
         "without it a second hand-driven run reports the sum of every run so far";
}

// The claim the whole task rests on: driving the C API by hand over the tape
// gives the fills BacktestRunner gives on the same tape.
TEST(CapiExecutorBarOhlc, HandDrivenBarsMatchTheRunner)
{
  const BarApi api = loadBarApi();
  ASSERT_TRUE(api.complete())
      << "the C bar path is incomplete; see CapiExecutorBarOhlc.TheBarPathIsReachableFromC";

  const std::vector<FillRow> expected = runnerFills();
  ASSERT_EQ(expected.size(), 2u) << "reference run changed shape:" << describe(expected);

  FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
  ASSERT_NE(exec, nullptr);
  driveTape(api, exec);

  const std::vector<FillRow> actual = readFills(exec);
  EXPECT_EQ(actual, expected) << "runner:" << describe(expected)
                              << "\nC API:" << describe(actual);

  flox_simulated_executor_destroy(exec);
}

// The mutation this is shaped against: on_bar_ohlc forwarding the close where
// the open belongs. A held order fills at the price the next bar opened at,
// not at the close of the bar its callback was shown.
TEST(CapiExecutorBarOhlc, AHeldOrderFillsAtTheOpenNotTheClose)
{
  const BarApi api = loadBarApi();
  ASSERT_TRUE(api.complete())
      << "the C bar path is incomplete; see CapiExecutorBarOhlc.TheBarPathIsReachableFromC";

  FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
  ASSERT_NE(exec, nullptr);
  driveTape(api, exec);

  const std::vector<FillRow> fills = readFills(exec);
  ASSERT_GE(fills.size(), 1u) << "the market buy never filled";
  const FillRow& buy = fills[0];
  ASSERT_EQ(buy.orderId, 1u);

  EXPECT_DOUBLE_EQ(buy.price, 106.0) << "a callback order fills at the next bar's open";
  EXPECT_NE(buy.price, 105.0) << "filled at the close of the bar the callback was shown";
  EXPECT_NE(buy.price, 110.0) << "filled at the high of a bar that had already closed";
  EXPECT_NE(buy.price, 90.0) << "filled at the low of a bar that had already closed";
  EXPECT_NE(buy.price, 100.0) << "filled at the open of a bar that had already closed";

  flox_simulated_executor_destroy(exec);
}

// The other half of the walk: a resting sell at 109 fills against bar 1's high
// of 109.5 and not against its close of 107, so the C bar path has to carry
// the intrabar extremes and not only the close.
TEST(CapiExecutorBarOhlc, ARestingOrderMatchesTheIntrabarExtreme)
{
  const BarApi api = loadBarApi();
  ASSERT_TRUE(api.complete())
      << "the C bar path is incomplete; see CapiExecutorBarOhlc.TheBarPathIsReachableFromC";

  FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
  ASSERT_NE(exec, nullptr);
  driveTape(api, exec);

  const std::vector<FillRow> fills = readFills(exec);
  ASSERT_EQ(fills.size(), 2u) << "the resting sell never matched the bar high:"
                              << describe(fills);
  const FillRow& sell = fills[1];
  EXPECT_EQ(sell.orderId, 2u);
  EXPECT_EQ(sell.side, 1u);
  EXPECT_DOUBLE_EQ(sell.price, 109.0) << "a resting limit trades at the price it posted";

  flox_simulated_executor_destroy(exec);
}

// Without the window the order is not held at all: it matches inside the bar
// it was submitted from, at a price the caller only knows because that bar has
// closed. Naming it separately keeps "the window is wired" from hiding behind
// "the fills match".
TEST(CapiExecutorBarOhlc, WithoutTheWindowTheOrderIsNotHeld)
{
  const BarApi api = loadBarApi();
  ASSERT_TRUE(api.complete())
      << "the C bar path is incomplete; see CapiExecutorBarOhlc.TheBarPathIsReachableFromC";

  FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
  ASSERT_NE(exec, nullptr);

  const BarRow& bar0 = tape()[0];
  flox_simulated_executor_advance_clock(exec, bar0.endNs);
  api.onBarOhlc(exec, kSym, bar0.open, bar0.high, bar0.low, bar0.close);
  flox_simulated_executor_submit_order(exec, /*id=*/1, /*side=buy=*/0, 0.0, 1.0,
                                       static_cast<uint8_t>(OrderType::MARKET), kSym);

  const std::vector<FillRow> fills = readFills(exec);
  ASSERT_EQ(fills.size(), 1u) << "an order submitted outside the window was held";
  EXPECT_DOUBLE_EQ(fills[0].price, bar0.close)
      << "outside the window the market has been walked to the close";

  flox_simulated_executor_destroy(exec);
}

// reset() is what makes a second hand-driven run report that run. Without it
// a binding replaying two parameter sets through one executor reads the sum.
TEST(CapiExecutorBarOhlc, ResetMakesASecondRunReportThatRun)
{
  const BarApi api = loadBarApi();
  ASSERT_TRUE(api.complete())
      << "the C bar path is incomplete; see CapiExecutorBarOhlc.TheBarPathIsReachableFromC";

  FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
  ASSERT_NE(exec, nullptr);

  driveTape(api, exec);
  const std::vector<FillRow> first = readFills(exec);
  ASSERT_FALSE(first.empty());

  api.reset(exec);
  EXPECT_EQ(flox_simulated_executor_fill_count(exec), 0u)
      << "reset left the previous run's fills behind";

  driveTape(api, exec);
  EXPECT_EQ(readFills(exec), first) << "the second run did not repeat the first";

  flox_simulated_executor_destroy(exec);
}

// Batch aggregation hands back a FloxBar with no close_reason, so the reason a
// bar closed -- the one field that separates a bar the threshold closed from
// one a flush forced out -- is readable on the live callback path
// (FloxBarData) and nowhere else. The struct is the ABI, so the field has to
// land there before any binding can carry it.
TEST(CapiExecutorBarOhlc, AggregatedBarsCarryACloseReason)
{
  EXPECT_TRUE(HasCloseReason<FloxBar>)
      << "needs `uint8_t close_reason;` on FloxBar, set from flox::Bar::reason in the "
         "batch aggregation path, so aggregated bars carry the same field the live "
         "FloxBarData already does";

  expectAggregatedCloseReason<FloxBar>();
}
