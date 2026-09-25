/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Backtest economics: what a run charges, pays and returns to the account.
//
// Five separate ledgers, each of which currently reports a number the rest of
// the engine contradicts:
//
//   1. Isolated-margin liquidation erases the leg without returning the
//      surviving posted margin to the account.
//   2. The first FundingSchedule::tick on a real unix timestamp settles every
//      8h boundary since 1970 instead of at most the one that just passed.
//   3. LatencyModel and the venue FeeSchedule exist, are configurable and are
//      exposed through every binding, but no fill is priced or timed by them.
//   4. The walk-forward runner never hands the in-sample winner's parameters
//      to the out-of-sample run.
//   5. The drawdown percentage is scaled to percent once by the result and a
//      second time by the report that prints it.

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/funding_schedule.h"
#include "flox/backtest/latency_model.h"
#include "flox/backtest/liquidation_engine.h"
#include "flox/backtest/optimization_stats.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/backtest/venue_stack.h"
#include "flox/backtest/walk_forward.h"
#include "flox/clearing/account.h"
#include "flox/clearing/fee_schedule.h"
#include "flox/engine/symbol_registry.h"
#include "flox/log/abstract_logger.h"
#include "flox/log/log_stream.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId kBtc = 1;
}  // namespace

// ===========================================================================
// 1. Isolated-margin liquidation returns the surviving margin
// ===========================================================================

// Isolated margin is a ring-fenced slice of the account: liquidation may take
// the position and everything the close cost, and nothing more. Whatever the
// slice still holds after the loss and the liquidation fee is the account's
// money and has to land back on the account balance.
//
// 100 contracts long from 100.00 with 2,000.00 of posted isolated margin. At a
// mark of 99.00 the leg is under its 25% maintenance requirement (2,475.00 on
// 9,900.00 of notional), so it force-closes. With no liquidation fee the close
// realises 100 * (99.00 - 100.00) = -100.00, leaving 1,900.00 of the posted
// margin intact.
//
// walkIsolatedAccount computes that remainder as `residualEquity` and then
// consults it only when it is NEGATIVE (to size the deficit for the insurance
// fund). A positive remainder is dropped on the floor together with the erased
// leg: the account ends the tick at 0.00 instead of 1,900.00, and the 1,900.00
// exists nowhere afterwards -- not on the account, not in the insurance fund.
TEST(BacktestEconomics, IsolatedLiquidationReturnsSurvivingMarginToTheAccount)
{
  LiquidationEngine engine;
  engine.addTier(0.0, 0.25);
  engine.setInsuranceFundCapital(1'000'000.0);
  engine.setAdlEnabled(false);
  engine.setLiquidationSlippageBps(0.0);  // no liquidation fee

  Account account(/*accountId=*/7, /*equity=*/0.0);
  account.setMarginMode(MarginMode::Isolated);
  account.openPosition(kBtc, /*quantity=*/100.0, /*entryPrice=*/100.0,
                       /*isolatedEquity=*/2000.0);
  engine.attachAccount(&account);

  const auto out = engine.onMark(kBtc, 99.00);

  ASSERT_EQ(out.liquidationsCount, 1u);
  EXPECT_EQ(account.positionCount(), 0u);

  // The close was solvent, so the insurance fund must not have moved.
  EXPECT_DOUBLE_EQ(out.insuranceFundDelta, 0.0);
  EXPECT_DOUBLE_EQ(engine.insuranceFundBalance(), 1'000'000.0);

  // 2,000.00 posted - 100.00 realised loss = 1,900.00 back to the account.
  EXPECT_DOUBLE_EQ(account.equity().toDouble(), 1900.00);
}

// The same remainder, net of the liquidation fee the engine charges. At 25 bps
// the close prints at 99.00 * (1 - 0.0025) = 98.752500 instead of 99.00, so the
// realised loss is 100 * (98.752500 - 100.00) = -124.750000 and the account
// must get back 2,000.00 - 124.750000 = 1,875.250000 -- not 1,900.00 (the fee
// has to be paid) and not 0.00 (the remainder has to come back).
TEST(BacktestEconomics, IsolatedLiquidationReturnsMarginNetOfTheLiquidationFee)
{
  LiquidationEngine engine;
  engine.addTier(0.0, 0.25);
  engine.setInsuranceFundCapital(1'000'000.0);
  engine.setAdlEnabled(false);
  engine.setLiquidationSlippageBps(25.0);

  Account account(/*accountId=*/7, /*equity=*/0.0);
  account.setMarginMode(MarginMode::Isolated);
  account.openPosition(kBtc, 100.0, 100.0, /*isolatedEquity=*/2000.0);
  engine.attachAccount(&account);

  const auto out = engine.onMark(kBtc, 99.00);

  ASSERT_EQ(out.liquidationsCount, 1u);
  EXPECT_DOUBLE_EQ(out.insuranceFundDelta, 0.0);
  EXPECT_NEAR(account.equity().toDouble(), 1875.250000, 1e-9);
}

// Control, green today: the cross-margin walk of the same economic position
// books the realised loss onto the account and leaves the rest of the balance
// standing. 2,000.00 of account equity, the same -100.00 close, 1,900.00 left.
// Isolated has to agree with this; only the path differs.
TEST(BacktestEconomics, CrossLiquidationKeepsTheSurvivingBalanceControl)
{
  LiquidationEngine engine;
  engine.addTier(0.0, 0.25);
  engine.setInsuranceFundCapital(1'000'000.0);
  engine.setAdlEnabled(false);
  engine.setLiquidationSlippageBps(0.0);

  Account account(/*accountId=*/8, /*equity=*/2000.0);
  account.setMarginMode(MarginMode::Cross);
  account.openPosition(kBtc, 100.0, 100.0);
  engine.attachAccount(&account);

  const auto out = engine.onMark(kBtc, 99.00);

  ASSERT_EQ(out.liquidationsCount, 1u);
  EXPECT_EQ(account.positionCount(), 0u);
  EXPECT_DOUBLE_EQ(out.insuranceFundDelta, 0.0);
  EXPECT_DOUBLE_EQ(account.equity().toDouble(), 1900.00);
}

// Guard, green today and it has to stay green: the remainder is only ever
// returned when there IS one. The same leg marked through to 70.00 realises
// 100 * (70.00 - 100.00) = -3,000.00 against 2,000.00 of posted margin, so the
// slice is wiped out and 1,000.00 short. That 1,000.00 is the insurance fund's
// to pay -- the account must end at 0.00, not at -1,000.00 (isolated margin
// caps the loss at the margin posted) and not at +1,000.00 (a sign slip while
// routing the remainder back would hand the trader the deficit as a credit).
TEST(BacktestEconomics, IsolatedLiquidationSendsANegativeRemainderToInsurance)
{
  LiquidationEngine engine;
  engine.addTier(0.0, 0.25);
  engine.setInsuranceFundCapital(1'000'000.0);
  engine.setAdlEnabled(false);
  engine.setLiquidationSlippageBps(0.0);

  Account account(/*accountId=*/9, /*equity=*/0.0);
  account.setMarginMode(MarginMode::Isolated);
  account.openPosition(kBtc, 100.0, 100.0, /*isolatedEquity=*/2000.0);
  engine.attachAccount(&account);

  const auto out = engine.onMark(kBtc, 70.00);

  ASSERT_EQ(out.liquidationsCount, 1u);
  EXPECT_DOUBLE_EQ(out.insuranceFundDelta, -1000.00);
  EXPECT_DOUBLE_EQ(engine.insuranceFundBalance(), 999'000.00);
  EXPECT_DOUBLE_EQ(account.equity().toDouble(), 0.00);
}

// ===========================================================================
// 2. The first funding tick on a real timestamp
// ===========================================================================

// A schedule that has never ticked has _lastTickNs == 0, i.e. 1970-01-01. The
// first tick of a backtest that stamps events with real exchange timestamps
// therefore asks the schedule to settle the whole interval (1970, 2026] in one
// call: at an 8h cadence that is 1,767,225,600 / 28,800 = 61,362 boundaries,
// every one of them settled against the SAME position and the SAME mark, for
// 61,362 funding payments on a position that has been open for one tick.
//
// The clamp that guards this fires at kMaxBoundariesPerTick = 100,000, so it
// never trips on any timestamp a real backtest will pass -- 2026 is 61,362
// boundaries, well under the bar, and a run would have to be dated past 2065
// before the guard engaged.
//
// The fix this was closed on was supposed to seed `_lastTickNs` to the first
// real timestamp, and to be covered by a test that a first tick on a real
// date emits at most one payment. Neither landed: the seed became a clamp two
// orders of magnitude above the value it had to catch, and no test exists
// (test_funding_schedule.cpp drives every case from a small base).
TEST(BacktestEconomics, FirstFundingTickOnARealTimestampSettlesAtMostOnce)
{
  // 2026-01-01T00:00:00Z.
  constexpr int64_t kNowNs = 1'767'225'600'000'000'000LL;
  constexpr int64_t kEightHoursNs = 8LL * 3600LL * 1'000'000'000LL;

  auto schedule = FundingSchedule::constant(kEightHoursNs, /*rate=*/0.0001);

  const std::vector<SymbolId> symbols{kBtc};
  const std::vector<double> positions{1.0};
  const std::vector<double> marks{100'000.0};

  const auto payments = schedule.tick(kNowNs, symbols, positions, marks);

  EXPECT_LE(payments.size(), 1u)
      << "the first tick settled " << payments.size()
      << " funding boundaries on a single open position";

  // A payment is -position * mark * rate = -1.0 * 100000.0 * 0.0001 = -10.00.
  // At most one of them, so the position can never owe more than 10.00 on the
  // tick that opened the schedule.
  double total = 0.0;
  for (const auto& p : payments)
  {
    total += p.amount;
  }
  EXPECT_GE(total, -10.00)
      << "the first tick charged " << total << " instead of at most -10.00";
}

// Control, green today: once the schedule is running, the cadence itself is
// right. Seed it at a 2026 timestamp, advance exactly 24 hours, and an 8h
// schedule settles exactly 3 boundaries for -30.00 total.
TEST(BacktestEconomics, FundingCadenceAfterTheFirstTickControl)
{
  constexpr int64_t kEightHoursNs = 8LL * 3600LL * 1'000'000'000LL;
  constexpr int64_t kStartNs = 1'767'225'600'000'000'000LL;

  auto schedule = FundingSchedule::constant(kEightHoursNs, 0.0001);
  const std::vector<SymbolId> symbols{kBtc};
  const std::vector<double> positions{1.0};
  const std::vector<double> marks{100'000.0};

  schedule.tick(kStartNs, symbols, positions, marks);  // seeding tick
  const auto payments =
      schedule.tick(kStartNs + 24LL * 3600LL * 1'000'000'000LL, symbols, positions, marks);

  EXPECT_EQ(payments.size(), 3u);
  double total = 0.0;
  for (const auto& p : payments)
  {
    total += p.amount;
  }
  EXPECT_NEAR(total, -30.00, 1e-9);
}

// ===========================================================================
// 3. Max drawdown is scaled to percent once
// ===========================================================================

namespace
{

Fill makeFill(OrderId id, Side side, double price, double qty, int64_t tsNs)
{
  Fill f;
  f.orderId = id;
  f.symbol = kBtc;
  f.side = side;
  f.price = Price::fromDouble(price);
  f.quantity = Quantity::fromDouble(qty);
  f.timestampNs = UnixNanos::fromRaw(tsNs);
  return f;
}

struct ReportParams
{
  std::string toString() const { return "p=1"; }
};

struct ReportGrid
{
  std::vector<ReportParams> params;
  size_t totalCombinations() const { return params.size(); }
  ReportParams operator[](size_t i) const { return params[i]; }
};

using ReportStats = OptimizationStatistics<ReportParams, ReportGrid>;

std::string readFile(const std::filesystem::path& p)
{
  std::ifstream in(p);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// An equity curve that starts at 100.00, closes a losing round trip at 90.00
// and then a winning one at 95.00: peak 100.00, trough 90.00, drawdown 10.00
// absolute and 10 percent, one win out of two trades for a 0.5 win rate.
//
// Both numbers are deliberately in the same report row: `winRate` is a
// fraction and its `* 100` in the report is correct, `maxDrawdownPct` is
// already a percent and its `* 100` is not. A fix that drops the scaling from
// the whole row trades one wrong number for another.
BacktestStats tenPercentDrawdownStats()
{
  BacktestConfig cfg;
  cfg.initialCapital = 100.0;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);
  result.recordFill(makeFill(1, Side::BUY, 100.0, 1.0, 1'000));
  result.recordFill(makeFill(2, Side::SELL, 90.0, 1.0, 2'000));
  result.recordFill(makeFill(3, Side::BUY, 90.0, 1.0, 3'000));
  result.recordFill(makeFill(4, Side::SELL, 95.0, 1.0, 4'000));
  return result.computeStats();
}

}  // namespace

// Control, green today: BacktestResult reports the drawdown in the unit the
// docs print it in. docs/how-to/backtest.md formats the same field as
// `f"DD {stats['max_drawdown_pct']:.2f}%"` -- straight into a percent sign
// with no scaling of its own -- so 100.00 -> 90.00 is 10.0, and the absolute
// drawdown next to it is 10.00 of account currency.
TEST(BacktestEconomics, MaxDrawdownIsPercentOnceInTheResultControl)
{
  const auto stats = tenPercentDrawdownStats();

  EXPECT_EQ(stats.totalTrades, 2u);
  EXPECT_NEAR(stats.maxDrawdown, 10.00, 1e-9);
  EXPECT_NEAR(stats.maxDrawdownPct, 10.0, 1e-9);
  EXPECT_NEAR(stats.winRate, 0.5, 1e-9);
  EXPECT_NEAR(stats.finalCapital, 95.00, 1e-9);
}

// The optimisation report scales that already-percent number to percent a
// second time. `optimization_stats.h` writes `(res.maxDrawdownPct() * 100)`
// into the Drawdown column (and `printSummary` does the same on the line it
// logs), so the 10 percent drawdown above is published as 1000.00% -- a
// drawdown ten times the size of the account, on the same page whose equity
// curve shows 100.00 -> 90.00.
//
// The Win Rate column beside it is the counter-example that fixes the unit:
// `winRate` really is a fraction, so its `* 100` is correct and must stay.
TEST(BacktestEconomics, OptimizationReportPrintsDrawdownAsPercentOnce)
{
  const auto stats = tenPercentDrawdownStats();
  ASSERT_NEAR(stats.maxDrawdownPct, 10.0, 1e-9) << "premise of this test";

  std::vector<OptimizationResult<ReportParams>> results(1);
  results[0].setFromStats(stats);

  const auto dir = std::filesystem::temp_directory_path() / "flox_bt_econ_report";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "report.md";

  ASSERT_TRUE(ReportStats::generateReport(results, path));
  const std::string content = readFile(path);

  EXPECT_EQ(content.find("1000.00%"), std::string::npos)
      << "a 10 percent drawdown was reported as 1000.00%";
  // Drawdown then win rate, in the row's own order: 10 percent and 50 percent.
  EXPECT_NE(content.find("| 10.00% | 50.00% |"), std::string::npos)
      << "report did not carry the 10.00% drawdown next to the 50.00% win "
         "rate; report was:\n"
      << content;

  std::filesystem::remove_all(dir);
}

// ===========================================================================
// 4. FeeSchedule tiers reach the fills
// ===========================================================================

namespace
{

SymbolId registerBtc(SymbolRegistry& reg)
{
  SymbolInfo info;
  info.exchange = "binance";
  info.symbol = "BTCUSDT";
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

// One round trip: market buy on the first trade, market sell on the fifth.
class RoundTripStrategy : public Strategy
{
 public:
  RoundTripStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, double qty)
      : Strategy(id, sym, reg), _sym(sym), _qty(qty)
  {
  }

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& /*ev*/) override
  {
    ++_seen;
    if (_seen == 1)
    {
      emitMarketBuy(_sym, Quantity::fromDouble(_qty));
    }
    else if (_seen == 5)
    {
      emitMarketSell(_sym, Quantity::fromDouble(_qty));
    }
  }

 private:
  SymbolId _sym;
  double _qty;
  int _seen{0};
};

std::filesystem::path writeFlatTape(SymbolId sym, size_t n, double price, double qty,
                                    const char* dirName)
{
  auto dir = std::filesystem::temp_directory_path() / dirName;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  constexpr int64_t kBaseNs = 1'767'225'600'000'000'000LL;  // 2026-01-01
  replay::WriterConfig cfg;
  cfg.output_dir = dir;
  cfg.create_index = false;
  replay::BinaryLogWriter writer(cfg);
  for (size_t i = 0; i < n; ++i)
  {
    replay::TradeRecord r{};
    r.exchange_ts_ns = kBaseNs + static_cast<int64_t>(i) * 1'000'000'000LL;
    r.recv_ts_ns = r.exchange_ts_ns;
    r.price_raw = Price::fromDouble(price).raw();
    r.qty_raw = Quantity::fromDouble(qty).raw();
    r.symbol_id = sym;
    r.side = 1;
    writer.writeTrade(r);
  }
  writer.close();
  return dir;
}

}  // namespace

// Control, green today: the ladder itself resolves the tier. 500,000,000.00 of
// 30-day notional on the stack's account puts a Binance UM stack in VIP 8
// (>= 400,000,000.00), whose published taker rate is 2.1 bps -- so a
// 100,000.00 fill is priced at 21.00, not the 200.00 a flat 20 bps charges.
TEST(BacktestEconomics, VenueFeeScheduleResolvesTheVolumeTierControl)
{
  auto stack = VenueStack::binance_um_futures(/*accountId=*/42, /*equity=*/10'000'000.0);
  stack.account().recordFill(/*tsNs=*/1'767'225'600'000'000'000LL,
                             /*notional=*/500'000'000.0);

  EXPECT_DOUBLE_EQ(stack.account().rollingNotional30d().toDouble(), 500'000'000.0);
  EXPECT_EQ(stack.fees().currentTierIndex(), 8u);

  const auto [makerBps, takerBps] = stack.fees().currentBps(1'767'225'600'000'000'000LL);
  EXPECT_DOUBLE_EQ(makerBps, 0.2);
  EXPECT_DOUBLE_EQ(takerBps, 2.1);
  EXPECT_NEAR(stack.fees().feeFor(1'767'225'600'000'000'000LL, 100'000.0, /*isMaker=*/false),
              21.00, 1e-9);
}

// A run driven by that stack has to pay that tier. The tiered-fee work was
// closed on the criterion "fee accounting in net_pnl honours per-fill tier",
// but the stack's FeeSchedule is never consulted by BacktestResult, which
// prices every fill off the flat BacktestConfig::feeRate
// (`backtest_result.cpp:424`, `backtest_runner.cpp:79`). The VIP-8 account
// above is billed at the flat rate: 20 bps here instead of 2.1 bps, a fee
// 9.52x the one the venue would charge.
TEST(BacktestEconomics, RunOnAVenueStackPaysThatStacksFeeTier)
{
  struct Case
  {
    double seededNotional30d;
    size_t tierIndex;
    double expectedFees;
    const char* tapeDir;
  };
  // Same round trip, same 200,000.00 of taker notional, two different tiers.
  // Regular (tier 0) charges 4.0 bps = 80.00; VIP 8 charges 2.1 bps = 42.00.
  // A rate read off the ladder moves between the two; a constant does not.
  const Case cases[] = {
      {0.0, 0u, 80.00, "flox_bt_econ_fee_tape_t0"},
      {500'000'000.0, 8u, 42.00, "flox_bt_econ_fee_tape_t8"},
  };

  for (const auto& c : cases)
  {
    SymbolRegistry reg;
    const SymbolId sym = registerBtc(reg);
    const auto tape = writeFlatTape(sym, /*n=*/10, /*price=*/50'000.0, /*qty=*/2.0,
                                    c.tapeDir);

    RoundTripStrategy strat(1, sym, reg, /*qty=*/2.0);

    auto stack = VenueStack::binance_um_futures(42, 10'000'000.0);
    if (c.seededNotional30d > 0.0)
    {
      stack.account().recordFill(1'767'225'600'000'000'000LL, c.seededNotional30d);
    }
    ASSERT_EQ(stack.fees().currentTierIndex(), c.tierIndex) << "premise of this case";

    BacktestConfig cfg;
    cfg.initialCapital = 10'000'000.0;
    cfg.feeRate = 0.0020;  // 20 bps flat -- deliberately neither tier's rate

    BacktestRunner runner(cfg);
    runner.setStrategy(&strat);
    runner.setSimulatedExecutor(&stack.executor(), &stack.clock());
    runner.runTape(tape);

    const auto res = runner.result();
    const auto stats = res.computeStats();

    ASSERT_EQ(res.fills().size(), 2u) << "one round trip must reach the result";
    double notional = 0.0;
    for (const auto& f : res.fills())
    {
      notional += (f.price * f.quantity).toDouble();
    }
    ASSERT_NEAR(notional, 200'000.00, 1e-6) << "two 100,000.00 sides";

    EXPECT_NEAR(stats.totalFees, c.expectedFees, 1e-6)
        << "tier " << c.tierIndex << ": the run paid " << stats.totalFees
        << " on 200,000.00 of notional; the stack's schedule prices it at "
        << c.expectedFees;

    std::filesystem::remove_all(tape);
  }
}

// ===========================================================================
// 5. LatencyModel reaches the fills
// ===========================================================================

namespace
{

void pushBook(SimulatedExecutor& exec, SymbolId sym, double bid, double bidQty,
              double ask, double askQty)
{
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid), Quantity::fromDouble(bidQty));
  asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(askQty));
  exec.onBookUpdate(sym, bids, asks);
}

Order marketBuy(OrderId id, SymbolId sym, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  o.timeInForce = TimeInForce::GTC;
  return o;
}

constexpr int64_t kSubmitNs = 1'767'225'600'000'000'000LL;  // 2026-01-01
constexpr int64_t kOrderDelayNs = 5'000'000;                // 5 ms

// The engine consumes a LatencyModel from the run configuration. The latency
// work was specified around exactly this field ("`BacktestConfig.latency`
// field, default `nullptr` (current behaviour)") and this effect ("order delay
// on Executor submit").
//
// needs: BacktestConfig::latency  (std::shared_ptr<LatencyModel>, default null)
// needs: SimulatedExecutor::applyConfig consumes it -- an order submitted at
//        T is not marketable until T + orderDelay(), and its fill is stamped
//        at the time it actually matched.
template <typename Cfg>
concept HasLatencyField = requires(Cfg& c) { c.latency; };

template <typename Cfg>
void checkOrderLatencyDefersTheFill()
{
  std::shared_ptr<LatencyModel> model =
      std::make_shared<ConstantLatency>(/*feed_ns=*/0, kOrderDelayNs, /*fill_ns=*/0);

  Cfg cfg{};
  if constexpr (std::is_assignable_v<decltype(cfg.latency)&, std::shared_ptr<LatencyModel>>)
  {
    cfg.latency = model;
  }
  else
  {
    cfg.latency = model.get();
  }

  SimulatedClock clock;
  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs));
  SimulatedExecutor exec(clock);
  exec.applyConfig(cfg);

  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  exec.submitOrder(marketBuy(1, kBtc, 1.0));

  EXPECT_TRUE(exec.fills().empty())
      << "the order filled at submit time despite a 5 ms order latency";

  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs + 4'000'000));
  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  EXPECT_TRUE(exec.fills().empty())
      << "the order filled 4 ms in, 1 ms short of the 5 ms it takes to reach "
         "the exchange";

  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs + kOrderDelayNs));
  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  ASSERT_EQ(exec.fills().size(), 1u) << "the order must fill once it arrives";
  EXPECT_EQ(exec.fills()[0].timestampNs.raw(), kSubmitNs + kOrderDelayNs);
}

// The delay has to come from the model, not from the fact that one is
// attached: a zero ConstantLatency must reproduce the instant baseline to the
// nanosecond.
template <typename Cfg>
void checkZeroLatencyModelStaysInstant()
{
  std::shared_ptr<LatencyModel> model = std::make_shared<ConstantLatency>(0, 0, 0);

  Cfg cfg{};
  if constexpr (std::is_assignable_v<decltype(cfg.latency)&, std::shared_ptr<LatencyModel>>)
  {
    cfg.latency = model;
  }
  else
  {
    cfg.latency = model.get();
  }

  SimulatedClock clock;
  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs));
  SimulatedExecutor exec(clock);
  exec.applyConfig(cfg);

  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  exec.submitOrder(marketBuy(1, kBtc, 1.0));

  ASSERT_EQ(exec.fills().size(), 1u)
      << "a zero-latency model must not defer the fill";
  EXPECT_EQ(exec.fills()[0].timestampNs.raw(), kSubmitNs);
}

}  // namespace

// Control, green today: with no latency configured a marketable order fills in
// the same call that submits it, stamped at the clock's current time. This is
// the "instant" baseline every backtest currently runs at, and it must stay
// the behaviour when no model is attached.
TEST(BacktestEconomics, NoLatencyModelFillsAtSubmitTimeControl)
{
  SimulatedClock clock;
  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs));
  SimulatedExecutor exec(clock);

  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  exec.submitOrder(marketBuy(1, kBtc, 1.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_EQ(exec.fills()[0].timestampNs.raw(), kSubmitNs);
}

// The latency work was closed as done on criteria that include "integration
// in Engine backtest path: feed delay on event timestamps, order delay on
// Executor submit, fill delay on OrderQueueTracker callback" and "end-to-end
// backtest with known input -> expected fill timing". None of that shipped:
// the four models exist and are reachable from every binding, but the only
// consumers in the tree are the C API and the QuickJS bridge, `BacktestConfig`
// has no `latency` field, and `SimulatedExecutor` has no setter for one.
// `docs/how-to/backtest-with-latency.md` has since been rewritten to say so
// ("These models are a sampling primitive ... `BacktestConfig` has no latency
// field"), which corrected the documentation rather than the engine: every
// backtest still runs at zero order latency.
TEST(BacktestEconomics, ConstantOrderLatencyDelaysTheFill)
{
  if constexpr (HasLatencyField<BacktestConfig>)
  {
    checkOrderLatencyDefersTheFill<BacktestConfig>();
    checkZeroLatencyModelStaysInstant<BacktestConfig>();
  }
  else
  {
    FAIL() << "BacktestConfig has no `latency` field: a LatencyModel cannot be "
              "attached to a run, so every backtest fills at zero order "
              "latency. Expected a 5 ms ConstantLatency to hold the fill until "
           << (kSubmitNs + kOrderDelayNs) << " ns.";
  }
}

// ===========================================================================
// 6. Walk-forward hands the in-sample winner to the out-of-sample run
// ===========================================================================

namespace
{

// One numeric parameter: how many trades to hold the position for. Buys on the
// first trade of its window and sells `hold` trades later.
class HoldNStrategy : public Strategy
{
 public:
  HoldNStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, int hold)
      : Strategy(id, sym, reg), _sym(sym), _hold(hold)
  {
  }

  int hold() const { return _hold; }

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& /*ev*/) override
  {
    ++_seen;
    if (_seen == 1)
    {
      emitMarketBuy(_sym, Quantity::fromDouble(1.0));
    }
    else if (_seen == 1 + _hold)
    {
      emitMarketSell(_sym, Quantity::fromDouble(1.0));
    }
  }

 private:
  SymbolId _sym;
  int _hold;
  int _seen{0};
};

// A price ramp of +1.00 per trade, so a longer hold is strictly more
// profitable in every window: the in-sample winner is always the largest hold
// on the grid, with no ties to break.
std::vector<OhlcvReplaySource::Bar> priceRamp(SymbolId sym, size_t n, double first,
                                              double slope = 1.0)
{
  std::vector<OhlcvReplaySource::Bar> bars;
  bars.reserve(n);
  constexpr int64_t kBaseNs = 1'767'225'600'000'000'000LL;
  for (size_t i = 0; i < n; ++i)
  {
    bars.push_back(
        {kBaseNs + static_cast<int64_t>(i) * 1'000'000'000LL,
         Price::fromDouble(first + slope * static_cast<double>(i)).raw(), sym});
  }
  return bars;
}

BacktestConfig walkForwardConfig()
{
  BacktestConfig cfg;
  cfg.initialCapital = 100'000.0;
  cfg.feeRate = 0.0;
  return cfg;
}

}  // namespace

// Control, green today: on this ramp the parameter ordering the walk-forward
// selection has to discover is unambiguous. Holding 1 / 2 / 3 trades out of a
// six-trade window returns exactly 1.00 / 2.00 / 3.00, so the in-sample winner
// of the grid {1, 2, 3} is 3 by a full 1.00 over the runner-up.
TEST(BacktestEconomics, HoldLengthRanksTheGridInSampleControl)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);

  // Rising ramp: 1.00 / 2.00 / 3.00, so the winner is the LAST grid point.
  // Falling ramp: -1.00 / -2.00 / -3.00, so the winner is the FIRST.
  const double rising[] = {1.00, 2.00, 3.00};
  const double falling[] = {-1.00, -2.00, -3.00};

  for (double slope : {1.0, -1.0})
  {
    const auto bars = priceRamp(sym, 6, 100.0, slope);
    for (int hold = 1; hold <= 3; ++hold)
    {
      HoldNStrategy strat(1, sym, reg, hold);
      BacktestRunner runner(walkForwardConfig());
      runner.setStrategy(&strat);
      OhlcvReplaySource source(bars);
      const auto stats = runner.run(source).computeStats();

      const double expected = (slope > 0.0) ? rising[hold - 1] : falling[hold - 1];
      EXPECT_EQ(stats.totalTrades, 1u) << "slope=" << slope << " hold=" << hold;
      EXPECT_NEAR(stats.totalPnl, expected, 1e-6)
          << "slope=" << slope << " hold=" << hold;
    }
  }
}

namespace
{

// The walk-forward runner optimises on the in-sample window and evaluates the
// winner out-of-sample. Without a parameter channel there is nothing to carry
// across the split, and the two runs are two independent copies of the same
// fixed strategy -- which is not walk-forward optimisation, it is the same
// backtest run twice on adjacent slices.
//
// needs: WalkForwardRunner::setParameterGrid(std::vector<std::vector<double>> axes)
// needs: WalkForwardRunner::setStrategyFactory(
//            MoveOnlyFunction<IStrategy*(std::size_t foldIndex,
//                                        const std::vector<double>& params)>)
//        -- the runner evaluates every grid point on the train slice, ranks
//        them, and builds the test strategy from the winning point.
template <typename Runner>
concept HasParameterGrid = requires(Runner& r) {
  r.setParameterGrid(std::vector<std::vector<double>>{});
};

// `slope` is the per-trade price step. On a rising ramp the longest hold on
// the grid wins in sample; on a falling one the shortest does. Running both
// pins the selection to the ranking, not to a fixed position in the grid.
template <typename Runner>
void checkWalkForwardHandsOverTheWinner(double slope, double expectedWinner,
                                        double expectedPnl)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto bars = priceRamp(sym, 16, 100.0, slope);

  WalkForwardConfig wf;
  wf.mode = WalkForwardMode::Sliding;
  wf.trainSize = 6;
  wf.testSize = 4;
  wf.step = 4;

  Runner runner(walkForwardConfig(), wf);
  runner.setParameterGrid(std::vector<std::vector<double>>{{1.0, 2.0, 3.0}});

  std::vector<std::unique_ptr<HoldNStrategy>> owned;
  std::vector<std::vector<double>> seen;
  runner.setStrategyFactory(
      [&](std::size_t /*foldIndex*/, const std::vector<double>& params) -> IStrategy*
      {
        seen.push_back(params);
        const int hold = params.empty() ? 1 : static_cast<int>(params[0]);
        owned.push_back(std::make_unique<HoldNStrategy>(1, sym, reg, hold));
        return owned.back().get();
      });

  const auto folds = runner.run(bars);
  ASSERT_EQ(folds.size(), 2u);

  // Every grid point has to be evaluated in-sample: 3 points x 2 folds, plus
  // one out-of-sample build per fold.
  int sawWinner = 0;
  for (const auto& p : seen)
  {
    ASSERT_EQ(p.size(), 1u);
    if (p[0] == expectedWinner)
    {
      ++sawWinner;
    }
  }
  EXPECT_GE(sawWinner, 3) << "the grid was never searched";

  // The last strategy built for each fold is the out-of-sample one: three
  // in-sample builds, then the test build, per fold.
  ASSERT_EQ(seen.size(), 8u);
  EXPECT_DOUBLE_EQ(seen[3][0], expectedWinner) << "fold 0 out-of-sample build";
  EXPECT_DOUBLE_EQ(seen[7][0], expectedWinner) << "fold 1 out-of-sample build";

  for (const auto& f : folds)
  {
    EXPECT_NEAR(f.trainStats.totalPnl, expectedPnl, 1e-6)
        << "fold " << f.foldIndex << " reported the train stats of a grid "
        << "point other than the winner";
    EXPECT_EQ(f.testStats.totalTrades, 1u) << "fold " << f.foldIndex;
    EXPECT_NEAR(f.testStats.totalPnl, expectedPnl, 1e-6)
        << "fold " << f.foldIndex << " ran out-of-sample on parameters other "
        << "than the in-sample winner";
  }
}

}  // namespace

// Anchored and sliding walk-forward were both closed as done on criteria that
// asked for the mode plus tests and docs. The windowing shipped; the
// optimisation did not, and neither did a test -- `grep -rl WalkForward
// tests/` is empty. `walk_forward.cpp:111` (and the three sibling call sites)
// builds the out-of-sample strategy with `_factory(f.foldIndex)`, the exact
// call it used for the in-sample strategy, so the only thing the train window
// hands to the test window is its fold number. A fold's `trainStats` and
// `testStats` are two runs of one unparameterised strategy on adjacent
// slices.
TEST(BacktestEconomics, WalkForwardRunsOutOfSampleOnTheInSampleWinner)
{
  if constexpr (HasParameterGrid<WalkForwardRunner>)
  {
    // Rising ramp: hold=3 wins in sample and returns 3.00 out of sample.
    checkWalkForwardHandsOverTheWinner<WalkForwardRunner>(
        /*slope=*/1.0, /*expectedWinner=*/3.0, /*expectedPnl=*/3.00);
    // Falling ramp, long-only strategy: hold=1 loses least, so the winner is
    // the FIRST grid point and the fold returns -1.00.
    checkWalkForwardHandsOverTheWinner<WalkForwardRunner>(
        /*slope=*/-1.0, /*expectedWinner=*/1.0, /*expectedPnl=*/-1.00);
  }
  else
  {
    FAIL() << "WalkForwardRunner has no parameter grid and its strategy factory "
              "takes only a fold index, so no in-sample result can reach the "
              "out-of-sample run. Expected both folds to evaluate the grid "
              "{1, 2, 3} in-sample and run out-of-sample on the winner (3), "
              "for a test PnL of 3.00 per fold instead of 1.00.";
  }
}

// ===========================================================================
// 7. The gaps a mutation run over the six fixes left open
// ===========================================================================
//
// Everything above pins the headline number of each fix. What follows pins
// the rules those numbers are computed under: which window the grid is ranked
// on, which overloads and modes carry the winner across the split, that the
// fee ladder is re-read as the run trades its way up it, which fee wins when
// two are configured, that the two latency components add up, and that the
// backstops (a clamped funding jump, a clamped negative delay, a schedule
// with no tiers, a grid point that skipped itself) still hold.

namespace
{

// A bar-driven twin of HoldNStrategy: buys on the first bar of its window and
// sells `hold` bars later. An order submitted from a bar callback is matched
// at the open of the NEXT bar, so a window needs hold + 2 bars for the round
// trip to close inside it.
class HoldNBarStrategy : public Strategy
{
 public:
  HoldNBarStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, int hold)
      : Strategy(id, sym, reg), _sym(sym), _hold(hold)
  {
  }

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    ++_seen;
    if (_seen == 1)
    {
      emitMarketBuy(_sym, Quantity::fromDouble(1.0));
    }
    else if (_seen == 1 + _hold)
    {
      emitMarketSell(_sym, Quantity::fromDouble(1.0));
    }
  }

 private:
  SymbolId _sym;
  int _hold;
  int _seen{0};
};

constexpr int64_t kBarBaseNs = 1'767'225'600'000'000'000LL;  // 2026-01-01

// An explicit price path, one close per bar. The ramps above are linear by
// construction; a fold that has to tell the train window from the test window
// needs the two halves to disagree.
std::vector<OhlcvReplaySource::Bar> barsFromPrices(SymbolId sym,
                                                   const std::vector<double>& prices)
{
  std::vector<OhlcvReplaySource::Bar> bars;
  bars.reserve(prices.size());
  for (size_t i = 0; i < prices.size(); ++i)
  {
    bars.push_back({kBarBaseNs + static_cast<int64_t>(i) * 1'000'000'000LL,
                    Price::fromDouble(prices[i]).raw(), sym});
  }
  return bars;
}

// The same path as full OHLCV bars, flat inside each bar so a fill lands on
// the bar's own price whichever edge of it matched.
std::vector<BarEvent> barEventsFromPrices(SymbolId sym,
                                          const std::vector<double>& prices)
{
  std::vector<BarEvent> bars;
  bars.reserve(prices.size());
  for (size_t i = 0; i < prices.size(); ++i)
  {
    const int64_t ts = kBarBaseNs + static_cast<int64_t>(i) * 60'000'000'000LL;
    const Price p = Price::fromDouble(prices[i]);
    BarEvent ev{};
    ev.symbol = sym;
    ev.barType = BarType::Time;
    ev.barTypeParam = 60'000'000'000ull;
    ev.bar.open = p;
    ev.bar.high = p;
    ev.bar.low = p;
    ev.bar.close = p;
    ev.bar.volume = Volume::fromDouble(100.0);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{ts}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{ts + 60'000'000'000LL}};
    bars.push_back(ev);
  }
  return bars;
}

// Records every parameter vector the runner asks for a strategy with, so a
// test can name the point the out-of-sample window was built from and not
// only the PnL it produced.
//
// `declineHoldOnce` makes the factory decline one grid point the documented
// way, by returning nullptr -- but only on the offer that declines it. If the
// runner comes back for the declined point anyway, the probe hands over a
// working strategy, so the fold finishes and the test can report which point
// was crowned instead of dying on a strategy that does not exist.
template <typename StrategyT>
struct WalkForwardProbe
{
  std::vector<std::unique_ptr<StrategyT>> owned;
  std::vector<std::vector<double>> seen;
  int declineHoldOnce{0};
  bool declined{false};

  auto factory(SymbolId sym, const SymbolRegistry& reg)
  {
    return [this, sym, &reg](std::size_t, const std::vector<double>& params) -> IStrategy*
    {
      seen.push_back(params);
      const int hold = params.empty() ? 1 : static_cast<int>(params[0]);
      if (declineHoldOnce != 0 && hold == declineHoldOnce && !declined)
      {
        declined = true;
        return nullptr;
      }
      owned.push_back(std::make_unique<StrategyT>(1, sym, reg, hold));
      return owned.back().get();
    };
  }
};

}  // namespace

// The grid is ranked on the TRAIN slice. On a path that rises for six bars and
// falls for four, the in-sample winner (the longest hold) is the worst point
// out of sample, so a ranking that reads the test window picks hold=1 and
// reports -1.00 on both halves -- the look-ahead the whole construction exists
// to prevent, and invisible on a monotone ramp where every window agrees.
TEST(BacktestEconomics, WalkForwardSlidingRanksTheGridOnTheTrainSliceOnly)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto bars = barsFromPrices(
      sym, {100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 105.0, 104.0, 103.0, 102.0});

  WalkForwardConfig wf;
  wf.mode = WalkForwardMode::Sliding;
  wf.trainSize = 6;
  wf.testSize = 4;
  wf.step = 4;

  WalkForwardRunner runner(walkForwardConfig(), wf);
  runner.setParameterGrid(std::vector<std::vector<double>>{{1.0, 2.0, 3.0}});
  WalkForwardProbe<HoldNStrategy> probe;
  runner.setStrategyFactory(probe.factory(sym, reg));

  const auto folds = runner.run(bars);
  ASSERT_EQ(folds.size(), 1u);
  ASSERT_EQ(probe.seen.size(), 4u) << "three in-sample builds, then the test build";
  ASSERT_EQ(probe.seen.back().size(), 1u);

  EXPECT_DOUBLE_EQ(probe.seen.back()[0], 3.0)
      << "out of sample on hold=" << probe.seen.back()[0]
      << ": the point that wins on the falling test slice, not the rising "
         "train slice's winner";

  // Train slice, hold=3: 100.00 -> 103.00.
  EXPECT_NEAR(folds[0].trainStats.totalPnl, 3.00, 1e-6)
      << "the grid was ranked on a window other than the train slice";
  // Test slice, the same hold=3 on the falling half: 105.00 -> 102.00.
  EXPECT_EQ(folds[0].testStats.totalTrades, 1u);
  EXPECT_NEAR(folds[0].testStats.totalPnl, -3.00, 1e-6)
      << "the out-of-sample window did not run on the in-sample winner";
}

// The anchored mode carries the winner across the split too. Its train window
// grows from bar 0 rather than sliding, but the handover is the same one, and
// it is a separate call site.
TEST(BacktestEconomics, WalkForwardAnchoredRunsOutOfSampleOnTheInSampleWinner)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto bars = priceRamp(sym, 10, 100.0, 1.0);

  WalkForwardConfig wf;
  wf.mode = WalkForwardMode::Anchored;
  wf.minTrainSize = 6;
  wf.testSize = 4;
  wf.step = 4;

  WalkForwardRunner runner(walkForwardConfig(), wf);
  runner.setParameterGrid(std::vector<std::vector<double>>{{1.0, 2.0, 3.0}});
  WalkForwardProbe<HoldNStrategy> probe;
  runner.setStrategyFactory(probe.factory(sym, reg));

  const auto folds = runner.run(bars);
  ASSERT_EQ(folds.size(), 1u);
  EXPECT_EQ(folds[0].trainStartBar, 0u);
  EXPECT_EQ(folds[0].trainEndBar, 6u);

  ASSERT_EQ(probe.seen.size(), 4u);
  ASSERT_EQ(probe.seen.back().size(), 1u);
  EXPECT_DOUBLE_EQ(probe.seen.back()[0], 3.0)
      << "the anchored test window was built from the first grid point, not "
         "from the winner";

  EXPECT_NEAR(folds[0].trainStats.totalPnl, 3.00, 1e-6);
  EXPECT_EQ(folds[0].testStats.totalTrades, 1u);
  EXPECT_NEAR(folds[0].testStats.totalPnl, 3.00, 1e-6)
      << "hold=3 returns 3.00 out of sample on this ramp; hold=1 returns 1.00";
}

// The BarEvent overload -- the one a run driven by real OHLCV bars takes --
// has its own pair of call sites, one per mode. A market order submitted from
// a bar callback matches at the next bar's open, so on a ramp of +1.00 per bar
// a hold of n still returns n and the winner of {1, 2, 3} is 3.
TEST(BacktestEconomics, WalkForwardOverBarEventsRunsOutOfSampleOnTheInSampleWinner)
{
  const std::vector<double> prices{100.0, 101.0, 102.0, 103.0, 104.0, 105.0,
                                   106.0, 107.0, 108.0, 109.0, 110.0};

  for (WalkForwardMode mode : {WalkForwardMode::Anchored, WalkForwardMode::Sliding})
  {
    SymbolRegistry reg;
    const SymbolId sym = registerBtc(reg);
    const auto bars = barEventsFromPrices(sym, prices);

    WalkForwardConfig wf;
    wf.mode = mode;
    wf.trainSize = 6;
    wf.minTrainSize = 6;
    wf.testSize = 5;
    wf.step = 5;

    WalkForwardRunner runner(walkForwardConfig(), wf);
    runner.setParameterGrid(std::vector<std::vector<double>>{{1.0, 2.0, 3.0}});
    WalkForwardProbe<HoldNBarStrategy> probe;
    runner.setStrategyFactory(probe.factory(sym, reg));

    const auto folds = runner.run(bars);
    const bool anchored = (mode == WalkForwardMode::Anchored);
    ASSERT_EQ(folds.size(), 1u) << "mode=" << (anchored ? "anchored" : "sliding");
    ASSERT_EQ(probe.seen.size(), 4u);
    ASSERT_EQ(probe.seen.back().size(), 1u);

    EXPECT_DOUBLE_EQ(probe.seen.back()[0], 3.0)
        << "mode=" << (anchored ? "anchored" : "sliding")
        << ": the bar-event test window was built from the first grid point";

    EXPECT_NEAR(folds[0].trainStats.totalPnl, 3.00, 1e-6)
        << "mode=" << (anchored ? "anchored" : "sliding");
    EXPECT_EQ(folds[0].testStats.totalTrades, 1u)
        << "mode=" << (anchored ? "anchored" : "sliding");
    EXPECT_NEAR(folds[0].testStats.totalPnl, 3.00, 1e-6)
        << "mode=" << (anchored ? "anchored" : "sliding")
        << ": hold=3 returns 3.00 out of sample here, hold=1 returns 1.00";
  }
}

// A factory returning nullptr is the documented way to decline a grid point.
// A declined point was never run, so it has no statistics to be ranked on and
// must not be crowned: here hold=3 declines, the win falls to hold=2, and the
// test window has to run on 2. Crowning the declined point instead carries the
// statistics of a window that was never run -- those of hold=2, the last point
// actually evaluated -- onto a point that earns 3.00 out of sample.
TEST(BacktestEconomics, WalkForwardSkipsADeclinedGridPointRatherThanCrowningIt)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto bars = priceRamp(sym, 10, 100.0, 1.0);

  WalkForwardConfig wf;
  wf.mode = WalkForwardMode::Sliding;
  wf.trainSize = 6;
  wf.testSize = 4;
  wf.step = 4;

  WalkForwardRunner runner(walkForwardConfig(), wf);
  runner.setParameterGrid(std::vector<std::vector<double>>{{1.0, 2.0, 3.0}});
  WalkForwardProbe<HoldNStrategy> probe;
  probe.declineHoldOnce = 3;  // the point that would otherwise win
  runner.setStrategyFactory(probe.factory(sym, reg));

  const auto folds = runner.run(bars);
  ASSERT_EQ(folds.size(), 1u);
  ASSERT_EQ(probe.seen.size(), 4u);
  ASSERT_EQ(probe.seen.back().size(), 1u);

  EXPECT_DOUBLE_EQ(probe.seen.back()[0], 2.0)
      << "the out-of-sample window was built from the declined point, which "
         "was crowned on the statistics of a window it never ran";
  EXPECT_NEAR(folds[0].trainStats.totalPnl, 2.00, 1e-6);
  EXPECT_EQ(folds[0].testStats.totalTrades, 1u);
  EXPECT_NEAR(folds[0].testStats.totalPnl, 2.00, 1e-6)
      << "out of sample on hold=3, the point the factory declined; hold=2 is "
         "the best point the fold actually evaluated";
}

// An axis with no candidates means "this parameter is not being searched", not
// "cancel the fold". Multiplying it through would zero the cartesian product
// and leave the fold with nothing to evaluate: no winner, empty train stats,
// and an out-of-sample run on whatever an empty parameter vector defaults to.
TEST(BacktestEconomics, WalkForwardSkipsAnEmptyGridAxisWithoutCancellingTheFold)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto bars = priceRamp(sym, 10, 100.0, 1.0);

  WalkForwardConfig wf;
  wf.mode = WalkForwardMode::Sliding;
  wf.trainSize = 6;
  wf.testSize = 4;
  wf.step = 4;

  WalkForwardRunner runner(walkForwardConfig(), wf);
  runner.setParameterGrid(
      std::vector<std::vector<double>>{{1.0, 2.0, 3.0}, std::vector<double>{}});
  WalkForwardProbe<HoldNStrategy> probe;
  runner.setStrategyFactory(probe.factory(sym, reg));

  const auto folds = runner.run(bars);
  ASSERT_EQ(folds.size(), 1u) << "the empty axis cancelled the fold";
  ASSERT_EQ(probe.seen.size(), 4u)
      << "the searched axis produced " << probe.seen.size()
      << " factory calls; the empty one should have been skipped, leaving "
         "three grid points and one test build";
  ASSERT_EQ(probe.seen.back().size(), 1u)
      << "the empty axis must not widen or empty the parameter vector";
  EXPECT_DOUBLE_EQ(probe.seen.back()[0], 3.0);

  EXPECT_EQ(folds[0].trainStats.totalTrades, 1u)
      << "the fold evaluated nothing in sample";
  EXPECT_NEAR(folds[0].trainStats.totalPnl, 3.00, 1e-6);
  EXPECT_NEAR(folds[0].testStats.totalPnl, 3.00, 1e-6);
}

// Two grid points that earn the same net PnL in sample are a tie, and the tie
// goes to the EARLIER point, so the selection does not depend on the order the
// cartesian product happens to enumerate in. Here holds 1 and 3 both return
// 1.00 on the train slice; out of sample they are 20.00 apart, so the rule is
// worth 20.00 a fold.
TEST(BacktestEconomics, WalkForwardBreaksAnInSampleTieToTheEarlierGridPoint)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  // Train: 100 -> 101 (hold 1: +1.00), -> 100 (hold 2: 0.00), -> 101 (hold 3:
  // +1.00, tied with hold 1). Test: hold 1 buys 100.00 and sells 110.00, hold
  // 3 buys 100.00 and sells 90.00.
  const auto bars = barsFromPrices(
      sym, {100.0, 101.0, 100.0, 101.0, 100.0, 100.0, 100.0, 110.0, 100.0, 90.0});

  WalkForwardConfig wf;
  wf.mode = WalkForwardMode::Sliding;
  wf.trainSize = 6;
  wf.testSize = 4;
  wf.step = 4;

  WalkForwardRunner runner(walkForwardConfig(), wf);
  runner.setParameterGrid(std::vector<std::vector<double>>{{1.0, 2.0, 3.0}});
  WalkForwardProbe<HoldNStrategy> probe;
  runner.setStrategyFactory(probe.factory(sym, reg));

  const auto folds = runner.run(bars);
  ASSERT_EQ(folds.size(), 1u);
  ASSERT_EQ(probe.seen.size(), 4u);
  ASSERT_EQ(probe.seen.back().size(), 1u);
  ASSERT_NEAR(folds[0].trainStats.totalPnl, 1.00, 1e-6) << "premise of this test";

  EXPECT_DOUBLE_EQ(probe.seen.back()[0], 1.0)
      << "the tie between hold=1 and hold=3 went to the later point";
  EXPECT_EQ(folds[0].testStats.totalTrades, 1u);
  EXPECT_NEAR(folds[0].testStats.totalPnl, 10.00, 1e-6)
      << "out of sample on the later of the two tied points: -10.00 instead "
         "of 10.00";
}

// The tier is resolved per fill, from the notional the run itself has traded,
// not once at the top of the replay. Seeded at 2,400,000.00 the account opens
// in VIP 1 (>= 250,000.00, taker 4.0 bps); the first 200,000.00 side is billed
// there for 80.00 and pushes the rolling window to 2,600,000.00, which is VIP
// 2 (>= 2,500,000.00, taker 3.5 bps), so the closing side costs 70.00. A
// ladder read once bills both sides at 4.0 bps for 160.00; the flat 20 bps of
// the config would charge 800.00.
TEST(BacktestEconomics, AVenueLadderIsRereadAsTheRunClimbsIt)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto tape = writeFlatTape(sym, /*n=*/10, /*price=*/50'000.0, /*qty=*/4.0,
                                  "flox_bt_econ_fee_tape_climb");

  RoundTripStrategy strat(1, sym, reg, /*qty=*/4.0);

  auto stack = VenueStack::binance_um_futures(42, 10'000'000.0);
  stack.account().recordFill(1'767'225'600'000'000'000LL, 2'400'000.0);
  ASSERT_EQ(stack.fees().currentTierIndex(), 1u) << "premise: the run opens in VIP 1";

  BacktestConfig cfg;
  cfg.initialCapital = 10'000'000.0;
  cfg.feeRate = 0.0020;

  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&stack.executor(), &stack.clock());
  runner.runTape(tape);

  const auto res = runner.result();
  const auto stats = res.computeStats();

  ASSERT_EQ(res.fills().size(), 2u);
  double notional = 0.0;
  for (const auto& f : res.fills())
  {
    notional += (f.price * f.quantity).toDouble();
  }
  ASSERT_NEAR(notional, 400'000.00, 1e-6) << "two 200,000.00 sides";

  EXPECT_NEAR(stats.totalFees, 150.00, 1e-6)
      << "the run paid " << stats.totalFees
      << " across a tier edge it crossed itself; 80.00 at 4.0 bps then 70.00 "
         "at 3.5 bps is 150.00";

  std::filesystem::remove_all(tape);
}

// extractResult() is the move-out twin of result() and has to price the run
// the same way. A run whose fills are extracted rather than copied is billed
// at the flat 20 bps -- 400.00 instead of the 80.00 the stack's tier 0 charges
// -- if the schedule is forwarded on only one of the two paths.
TEST(BacktestEconomics, ExtractResultPricesTheRunWithTheVenueLadder)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto tape = writeFlatTape(sym, /*n=*/10, /*price=*/50'000.0, /*qty=*/2.0,
                                  "flox_bt_econ_fee_tape_extract");

  RoundTripStrategy strat(1, sym, reg, /*qty=*/2.0);

  auto stack = VenueStack::binance_um_futures(42, 10'000'000.0);
  ASSERT_EQ(stack.fees().currentTierIndex(), 0u);

  BacktestConfig cfg;
  cfg.initialCapital = 10'000'000.0;
  cfg.feeRate = 0.0020;

  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&stack.executor(), &stack.clock());
  runner.runTape(tape);

  auto res = runner.extractResult();
  const auto stats = res.computeStats();

  ASSERT_EQ(res.fills().size(), 2u);
  EXPECT_NEAR(stats.totalFees, 80.00, 1e-6)
      << "extractResult() billed the run at " << stats.totalFees
      << "; tier 0 prices 200,000.00 of taker notional at 4.0 bps = 80.00";

  std::filesystem::remove_all(tape);
}

// A caller that replaces the fee model outright -- usePercentageFee false,
// a fixed charge per trade -- means it. The venue ladder is the default the
// flat config rate is upgraded from, not an override of an explicit choice:
// two fills at 7.00 are 14.00, whatever the stack's tier would have charged.
TEST(BacktestEconomics, AnExplicitFixedFeeOutranksTheVenueLadder)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto tape = writeFlatTape(sym, /*n=*/10, /*price=*/50'000.0, /*qty=*/2.0,
                                  "flox_bt_econ_fee_tape_fixed");

  RoundTripStrategy strat(1, sym, reg, /*qty=*/2.0);

  auto stack = VenueStack::binance_um_futures(42, 10'000'000.0);
  ASSERT_GT(stack.fees().tierCount(), 0u) << "premise: there is a ladder to outrank";

  BacktestConfig cfg;
  cfg.initialCapital = 10'000'000.0;
  cfg.feeRate = 0.0020;
  cfg.usePercentageFee = false;
  cfg.fixedFeePerTrade = 7.00;

  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&stack.executor(), &stack.clock());
  runner.runTape(tape);

  const auto stats = runner.result().computeStats();

  EXPECT_NEAR(stats.totalFees, 14.00, 1e-6)
      << "the run paid " << stats.totalFees
      << " with a fixed fee of 7.00 per trade configured; the stack's tier 0 "
         "would have charged 80.00";

  std::filesystem::remove_all(tape);
}

// A schedule with no tiers resolves to no rate at all, so consulting it would
// price every fill at 0.00 and silently make the run free. The guard is what
// keeps such a schedule falling through to the flat config rate: 20 bps on
// 200,000.00 is 400.00.
TEST(BacktestEconomics, AVenueScheduleWithNoTiersFallsBackToTheFlatRate)
{
  SymbolRegistry reg;
  const SymbolId sym = registerBtc(reg);
  const auto tape = writeFlatTape(sym, /*n=*/10, /*price=*/50'000.0, /*qty=*/2.0,
                                  "flox_bt_econ_fee_tape_empty_ladder");

  RoundTripStrategy strat(1, sym, reg, /*qty=*/2.0);

  FeeSchedule empty;
  ASSERT_EQ(empty.tierCount(), 0u);

  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setFeeSchedule(&empty);

  BacktestConfig cfg;
  cfg.initialCapital = 10'000'000.0;
  cfg.feeRate = 0.0020;

  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&exec, &clock);
  runner.runTape(tape);

  const auto res = runner.result();
  const auto stats = res.computeStats();

  ASSERT_EQ(res.fills().size(), 2u);
  double notional = 0.0;
  for (const auto& f : res.fills())
  {
    notional += (f.price * f.quantity).toDouble();
  }
  ASSERT_NEAR(notional, 200'000.00, 1e-6);

  EXPECT_NEAR(stats.totalFees, 400.00, 1e-6)
      << "the run paid " << stats.totalFees
      << " against a schedule with no tiers; an empty ladder has no rate to "
         "quote, so the flat 20 bps must still apply";

  std::filesystem::remove_all(tape);
}

namespace
{

// A model whose jitter band is wider than its median, which is what a
// realistic profile drawn around a small mean looks like: some draws come back
// negative. An order cannot arrive before it was sent, so the executor has to
// floor the draw at zero rather than subtract it from the venue's own ack.
class NegativeDrawLatency final : public LatencyModel
{
 public:
  explicit NegativeDrawLatency(int64_t orderNs) : _orderNs(orderNs) {}

  int64_t feedDelay() override { return 0; }
  int64_t orderDelay() override { return _orderNs; }
  int64_t fillDelay() override { return 0; }

 private:
  int64_t _orderNs;
};

}  // namespace

// The wire model and the venue's own submit-ack profile are two separate
// delays on the same order and they add up: an order submitted at T with a
// 5 ms model and a 3 ms ack reaches the matching engine at T + 8 ms, not at
// T + 5 ms (the ack dropped) and not at T + 3 ms (the model dropped).
TEST(BacktestEconomics, OrderLatencyAndSubmitAckLatencyCompose)
{
  constexpr int64_t kAckNs = 3'000'000;  // 3 ms

  BacktestConfig cfg;
  cfg.latency = std::make_shared<ConstantLatency>(/*feed_ns=*/0, kOrderDelayNs,
                                                  /*fill_ns=*/0);
  cfg.submitAckLatencyNs = kAckNs;
  cfg.submitAckJitterNs = 0;

  SimulatedClock clock;
  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs));
  SimulatedExecutor exec(clock);
  exec.applyConfig(cfg);

  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  exec.submitOrder(marketBuy(1, kBtc, 1.0));
  EXPECT_TRUE(exec.fills().empty()) << "the order filled at submit time";

  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs + kOrderDelayNs));
  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  EXPECT_TRUE(exec.fills().empty())
      << "the order filled after the 5 ms wire delay alone, with the venue's "
         "3 ms submit ack dropped";

  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs + kOrderDelayNs + kAckNs));
  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  ASSERT_EQ(exec.fills().size(), 1u) << "the order never arrived";
  EXPECT_EQ(exec.fills()[0].timestampNs.raw(), kSubmitNs + kOrderDelayNs + kAckNs);
}

// A negative draw is floored at zero, not carried into the sum. With a 5 ms
// submit ack and a model that draws -2 ms the order still arrives at T + 5 ms;
// an unclamped draw would land it at T + 3 ms, two milliseconds of the venue's
// own latency refunded by a jitter sample.
TEST(BacktestEconomics, ANegativeOrderDelayDrawNeverOutrunsTheSubmitAck)
{
  constexpr int64_t kAckNs = 5'000'000;            // 5 ms
  constexpr int64_t kNegativeDrawNs = -2'000'000;  // -2 ms

  BacktestConfig cfg;
  cfg.latency = std::make_shared<NegativeDrawLatency>(kNegativeDrawNs);
  cfg.submitAckLatencyNs = kAckNs;
  cfg.submitAckJitterNs = 0;

  SimulatedClock clock;
  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs));
  SimulatedExecutor exec(clock);
  exec.applyConfig(cfg);

  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  exec.submitOrder(marketBuy(1, kBtc, 1.0));
  EXPECT_TRUE(exec.fills().empty()) << "the order filled at submit time";

  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs + kAckNs + kNegativeDrawNs));
  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  EXPECT_TRUE(exec.fills().empty())
      << "a negative draw was subtracted from the submit ack: the order "
         "arrived 2 ms early";

  clock.advanceTo(UnixNanos::fromRaw(kSubmitNs + kAckNs));
  pushBook(exec, kBtc, 99.99, 10.0, 100.01, 10.0);
  ASSERT_EQ(exec.fills().size(), 1u) << "the order never arrived";
  EXPECT_EQ(exec.fills()[0].timestampNs.raw(), kSubmitNs + kAckNs);
}

// Seeding the cursor fixes the first tick; later ticks can still be handed a
// jump -- a resumed run, a gap in the tape -- and the clamp is what keeps the
// payment list bounded when they are. At a 1 ms cadence a leap of 200,000
// intervals settles the last 100,000 boundaries and no more.
TEST(BacktestEconomics, ALaterFundingTickClampsAnUnboundedJump)
{
  constexpr int64_t kIntervalNs = 1'000'000;  // 1 ms
  constexpr int64_t kStartNs = 1'767'225'600'000'000'000LL;
  constexpr size_t kMaxBoundaries = 100'000;  // FundingSchedule::kMaxBoundariesPerTick

  auto schedule = FundingSchedule::constant(kIntervalNs, /*rate=*/0.0001);
  const std::vector<SymbolId> symbols{kBtc};
  const std::vector<double> positions{1.0};
  const std::vector<double> marks{100'000.0};

  const auto seeding = schedule.tick(kStartNs, symbols, positions, marks);
  ASSERT_LE(seeding.size(), 1u) << "premise: the first tick is seeded";

  const auto payments = schedule.tick(
      kStartNs + 200'000LL * kIntervalNs, symbols, positions, marks);

  EXPECT_EQ(payments.size(), kMaxBoundaries)
      << "a jump of 200,000 boundaries settled " << payments.size()
      << " of them; the clamp keeps the last 100,000";
}

namespace
{

class CapturingLogger final : public ILogger
{
 public:
  void info(std::string_view msg) override
  {
    _text.append(msg);
    _text.push_back('\n');
  }
  void warn(std::string_view msg) override { info(msg); }
  void error(std::string_view msg) override { info(msg); }

  const std::string& text() const { return _text; }

 private:
  std::string _text;
};

}  // namespace

// The report file and the summary line print the same row and have to agree.
// `generateReport` is fixed above; `printSummary` writes the drawdown into the
// log, where a second `* 100` publishes the same 10 percent drawdown as
// 1000% to anyone reading the run's output. The WinRate beside it is a
// fraction and keeps its scaling.
TEST(BacktestEconomics, OptimizationSummaryLogsDrawdownAsPercentOnce)
{
  const auto stats = tenPercentDrawdownStats();
  ASSERT_NEAR(stats.maxDrawdownPct, 10.0, 1e-9) << "premise of this test";

  std::vector<OptimizationResult<ReportParams>> results(1);
  results[0].setFromStats(stats);

  CapturingLogger logger;
  setGlobalLogger(&logger);
  ReportStats::printSummary(results);
  setGlobalLogger(nullptr);

  const std::string& text = logger.text();
  ASSERT_NE(text.find("Best:"), std::string::npos)
      << "printSummary logged nothing; captured:\n"
      << text;
  EXPECT_EQ(text.find("DD=1000%"), std::string::npos)
      << "a 10 percent drawdown was logged as 1000%; captured:\n"
      << text;
  EXPECT_NE(text.find("DD=10%"), std::string::npos)
      << "the summary line did not carry the 10 percent drawdown; captured:\n"
      << text;
  EXPECT_NE(text.find("WinRate=50%"), std::string::npos)
      << "the win rate beside it is a fraction and must stay scaled; "
         "captured:\n"
      << text;
}
