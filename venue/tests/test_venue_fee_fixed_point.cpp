/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A fee is money, and money in this venue is a fixed-point raw.
 *
 * engine::Fees prices a print by leaving the raws: the notional is divided
 * down into a double, the rate is applied to the double, and the answer is
 * pushed back through Volume::fromDouble. Past 2^53 raw -- about 9e7 quote
 * units, an ordinary block on a liquid instrument -- the notional no longer
 * survives the trip, and the half-up rounding fromDouble applies at the end is
 * applied to a number that is already wrong. The result is a FeeCharged in the
 * replayed event stream, and a ledger move behind it, that depends on the
 * floating-point arithmetic the two ends happened to be built with.
 *
 * The print used below is picked so the disagreement cannot be argued away as
 * a choice of rounding rule: the exact fee lands at .4995 and .4985 of a raw,
 * so truncation toward zero and round-to-nearest give the SAME integer, and
 * the double path still returns one raw more than either. Every expectation
 * here is therefore a statement about the arithmetic, not about a convention.
 */
#include "flox-venue/engine/fees.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"
#include "flox-venue/symbol_config.h"

#include "flox/clearing/fee_schedule.h"
#include "flox/util/base/scale_check.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::engine::Fees;
using flox::venue::engine::kFeeRateScale;

namespace
{

constexpr SymbolId SYM = 7;
constexpr AssetId BASE = 1;
constexpr AssetId QUOTE = 2;
constexpr uint64_t VENUE_ACCT = 999;

// The print. 4900.00000179 units at 67123.45678901 -- a 3.28e8 quote-unit
// block, i.e. a notional raw above 2^53, which is where the double path stops
// being able to hold it.
constexpr int64_t kPriceRaw = 6'712'345'678'901LL;
constexpr int64_t kQtyRaw = 490'000'000'179LL;

// price x quantity at the symbol's scales, the way ledger.h computes it.
constexpr int64_t kNotionalRaw = 32'890'493'838'629'998LL;

// The two rates, in tenths of a basis point so they are integers here: 2.5 bps
// maker, 7.5 bps taker. Different on purpose -- a fix that charges one side's
// rate to both has to fail.
constexpr int64_t kMakerBpsTenths = 25;
constexpr int64_t kTakerBpsTenths = 75;

// notionalRaw * bps / 10000, exactly:
//   32890493838629998 * 25  / 100000 =  8222623459657.4995
//   32890493838629998 * 75  / 100000 = 24667870378972.4985
constexpr int64_t kMakerFeeRaw = 8'222'623'459'657LL;
constexpr int64_t kTakerFeeRaw = 24'667'870'378'972LL;

// The denominator the two numerators above are taken over: 10000 bps, times
// the 10 that carries the half basis point.
constexpr int64_t kBpsTenthsScale = 100'000;

double bpsOf(int64_t tenths) { return static_cast<double>(tenths) / 10.0; }

// A schedule written in basis points directly, for the rates whose point is
// what happens BELOW a tenth of a bp.
flox::FeeSchedule scheduleBps(double makerBps, double takerBps)
{
  flox::FeeSchedule fs;
  fs.addTier(0.0, makerBps, takerBps);
  return fs;
}

flox::FeeSchedule schedule(int64_t makerTenths, int64_t takerTenths)
{
  flox::FeeSchedule fs;
  fs.addTier(0.0, bpsOf(makerTenths), bpsOf(takerTenths));
  return fs;
}

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromRaw(1);
  c.minPrice = Price::fromRaw(1);
  c.maxPrice = Price::fromRaw(100'000'00000000LL);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  return c;
}

Trade print()
{
  Trade t;
  t.symbol = SYM;
  t.price = Price::fromRaw(kPriceRaw);
  t.quantity = Quantity::fromRaw(kQtyRaw);
  t.makerId = 10;
  t.takerId = 20;
  t.makerAccount = 1;
  t.takerAccount = 2;
  t.takerSide = Side::BUY;
  t.tradeId = 1;
  return t;
}

NewOrder limitOrder(OrderId id, Side side, int64_t priceRaw, int64_t qtyRaw, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromRaw(priceRaw);
  o.quantity = Quantity::fromRaw(qtyRaw);
  o.accountId = acct;
  return o;
}

// A print of exactly one unit, so the notional raw IS the price raw and the
// arithmetic under test is the rate, not the notional.
Trade unitPrint(int64_t priceRaw)
{
  Trade t = print();
  t.price = Price::fromRaw(priceRaw);
  t.quantity = Quantity::fromRaw(Quantity::Scale);
  return t;
}

std::vector<const FeeCharged*> feesIn(const std::vector<OutboundEvent>& out)
{
  std::vector<const FeeCharged*> v;
  for (const OutboundEvent& e : out)
  {
    if (const auto* f = std::get_if<FeeCharged>(&e))
    {
      v.push_back(f);
    }
  }
  return v;
}

// The two fees a print produces on `c`, maker first.
std::pair<int64_t, int64_t> chargedOn(flox::FeeSchedule fs, const Trade& t, const SymbolConfig& c,
                                      int64_t nowRaw = 0)
{
  Fees f;
  f.setSchedule(std::move(fs));
  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };
  f.emit(t, c, nowRaw, sink);
  const auto fees = feesIn(out);
  if (fees.size() != 2)
  {
    return {0, 0};
  }
  return {fees[0]->fee.raw(), fees[1]->fee.raw()};
}

// The same on the default symbol.
std::pair<int64_t, int64_t> chargedBy(flox::FeeSchedule fs, const Trade& t, int64_t nowRaw = 0)
{
  return chargedOn(std::move(fs), t, cfg(), nowRaw);
}

}  // namespace

// The arithmetic the expectations above stand on, stated once so the numbers
// are checkable without trusting the comment: the notional is what ledger.h
// computes, and the exact fee is more than a third of a raw away from the
// midpoint, so no rounding rule reaches the double path's answer.
TEST(VenueFeeFixedPoint, TheExpectedRawsAreNotAChoiceOfRoundingRule)
{
  const __int128 notional =
      static_cast<__int128>(kPriceRaw) * kQtyRaw / static_cast<__int128>(Price::Scale);
  EXPECT_EQ(static_cast<int64_t>(notional), kNotionalRaw);

  const __int128 makerNum = notional * kMakerBpsTenths;
  const __int128 takerNum = notional * kTakerBpsTenths;
  EXPECT_EQ(static_cast<int64_t>(makerNum / kBpsTenthsScale), kMakerFeeRaw);
  EXPECT_EQ(static_cast<int64_t>(takerNum / kBpsTenthsScale), kTakerFeeRaw);

  // Truncation and round-to-nearest agree: both remainders are below half the
  // denominator, so kMakerFeeRaw / kTakerFeeRaw are the answer either way.
  EXPECT_LT(static_cast<int64_t>(makerNum % kBpsTenthsScale) * 2, kBpsTenthsScale);
  EXPECT_LT(static_cast<int64_t>(takerNum % kBpsTenthsScale) * 2, kBpsTenthsScale);
}

// The report path names the exact fee. This is the number that goes into the
// event stream and into the replay digest.
TEST(VenueFeeFixedPoint, AReportedFeeIsTheExactShareOfTheNotional)
{
  Fees f;
  f.setSchedule(schedule(kMakerBpsTenths, kTakerBpsTenths));

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };
  f.emit(print(), cfg(), 0, sink);

  const auto fees = feesIn(out);
  ASSERT_EQ(fees.size(), 2U);
  EXPECT_TRUE(fees[0]->maker);
  EXPECT_FALSE(fees[1]->maker);
  EXPECT_EQ(fees[0]->fee.raw(), kMakerFeeRaw);
  EXPECT_EQ(fees[1]->fee.raw(), kTakerFeeRaw);
}

// The settlement path charges what it reports, and what it reports is exact.
TEST(VenueFeeFixedPoint, ASettledFeeMovesTheExactRaw)
{
  Fees f;
  f.setSchedule(schedule(kMakerBpsTenths, kTakerBpsTenths));

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };

  Ledger led;
  const Amount seed = static_cast<Amount>(1'000'000'000'000'000LL);
  led.deposit(1, QUOTE, seed);
  led.deposit(2, QUOTE, seed);

  f.settle(print(), cfg(), 0, led, VENUE_ACCT, sink);

  EXPECT_EQ(led.available(1, QUOTE), seed - kMakerFeeRaw);
  EXPECT_EQ(led.available(2, QUOTE), seed - kTakerFeeRaw);
  EXPECT_EQ(led.available(VENUE_ACCT, QUOTE),
            static_cast<Amount>(kMakerFeeRaw) + kTakerFeeRaw);
}

// A thousand identical prints. One raw of drift per print is invisible in any
// single FeeCharged and is a whole quote unit by the end of the run; a venue
// that closes its books against this number has to be able to add it up.
TEST(VenueFeeFixedPoint, AThousandFeesSumToTheExactRaw)
{
  constexpr int kPrints = 1000;

  Fees f;
  f.setSchedule(schedule(kMakerBpsTenths, kTakerBpsTenths));

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };

  Ledger led;
  const Amount seed = static_cast<Amount>(1'000'000'000'000'000'000LL);
  led.deposit(1, QUOTE, seed);
  led.deposit(2, QUOTE, seed);

  for (int i = 0; i < kPrints; ++i)
  {
    f.settle(print(), cfg(), 0, led, VENUE_ACCT, sink);
  }

  const Amount makerTotal = static_cast<Amount>(kMakerFeeRaw) * kPrints;
  const Amount takerTotal = static_cast<Amount>(kTakerFeeRaw) * kPrints;
  EXPECT_EQ(led.available(1, QUOTE), seed - makerTotal);
  EXPECT_EQ(led.available(2, QUOTE), seed - takerTotal);
  EXPECT_EQ(led.available(VENUE_ACCT, QUOTE), makerTotal + takerTotal);

  // And the stream says the same thing the ledger does.
  const auto fees = feesIn(out);
  ASSERT_EQ(fees.size(), static_cast<size_t>(2 * kPrints));
  __int128 streamed = 0;
  for (const FeeCharged* fc : fees)
  {
    streamed += fc->fee.raw();
  }
  EXPECT_EQ(streamed, makerTotal + takerTotal);
}

// The venue module needs a native 128-bit integer (root CMakeLists.txt: the
// module is disabled on a toolchain without one), so it cannot be configured
// with FLOX_FORCE_PORTABLE_INT128 the way the fixed-point suite is. The two
// paths are therefore selected per call site instead, in one binary, exactly
// as mulDivI64As was written for: the fee has to be the same number on both,
// and the same number the venue charged.
TEST(VenueFeeFixedPoint, TheFeeIsTheSameOnBothArithmeticPaths)
{
  const int64_t nativeMaker = mulDivI64As<false>(kNotionalRaw, kMakerBpsTenths, kBpsTenthsScale);
  const int64_t portableMaker = mulDivI64As<true>(kNotionalRaw, kMakerBpsTenths, kBpsTenthsScale);
  const int64_t nativeTaker = mulDivI64As<false>(kNotionalRaw, kTakerBpsTenths, kBpsTenthsScale);
  const int64_t portableTaker = mulDivI64As<true>(kNotionalRaw, kTakerBpsTenths, kBpsTenthsScale);

  EXPECT_EQ(nativeMaker, portableMaker);
  EXPECT_EQ(nativeTaker, portableTaker);
  EXPECT_EQ(nativeMaker, kMakerFeeRaw);
  EXPECT_EQ(nativeTaker, kTakerFeeRaw);

  Fees f;
  f.setSchedule(schedule(kMakerBpsTenths, kTakerBpsTenths));
  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };
  f.emit(print(), cfg(), 0, sink);

  const auto fees = feesIn(out);
  ASSERT_EQ(fees.size(), 2U);
  EXPECT_EQ(fees[0]->fee.raw(), portableMaker);
  EXPECT_EQ(fees[1]->fee.raw(), portableTaker);
}

// The same print through the engine, so the number is the one a client and a
// replay actually see rather than one a component produced in isolation.
TEST(VenueFeeFixedPoint, TheEngineStreamCarriesTheExactFee)
{
  std::vector<OutboundEvent> out;
  Ledger led;
  MatchingEngine<MatchingBook> e(cfg(), [&out](const OutboundEvent& ev)
                                 { out.push_back(ev); });
  e.setLedger(&led, VENUE_ACCT);
  e.setFeeSchedule(schedule(kMakerBpsTenths, kTakerBpsTenths));
  led.deposit(1, BASE, static_cast<Amount>(1'000'000'000'000'000LL));
  led.deposit(2, QUOTE, static_cast<Amount>(1'000'000'000'000'000'000LL));

  e.submit(InboundCommand{limitOrder(1, Side::SELL, kPriceRaw, kQtyRaw, 1)}, 1);
  e.submit(InboundCommand{limitOrder(2, Side::BUY, kPriceRaw, kQtyRaw, 2)}, 2);

  const auto fees = feesIn(out);
  ASSERT_EQ(fees.size(), 2U);
  EXPECT_EQ(fees[0]->fee.raw(), kMakerFeeRaw);
  EXPECT_EQ(fees[1]->fee.raw(), kTakerFeeRaw);
}

// The control. A print small enough that the notional still fits a double
// exactly is priced the same either way, and stays priced that way: 200 quote
// units at 1 bp and 5 bps is 0.02 and 0.10, to the raw.
TEST(VenueFeeFixedPoint, ASmallPrintIsAlreadyExactAndStaysThatWay)
{
  Fees f;
  f.setSchedule(schedule(/*1.0 bps*/ 10, /*5.0 bps*/ 50));

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };

  Trade t = print();
  t.price = Price::fromRaw(100'00000000LL);
  t.quantity = Quantity::fromRaw(2'00000000LL);
  f.emit(t, cfg(), 0, sink);

  const auto fees = feesIn(out);
  ASSERT_EQ(fees.size(), 2U);
  EXPECT_EQ(fees[0]->fee.raw(), 2'000'000);   // 0.02
  EXPECT_EQ(fees[1]->fee.raw(), 10'000'000);  // 0.10
}

// ---- the rate's own boundary ---------------------------------------------
//
// A tier is written in basis points by a human and arrives as a double, so it
// crosses into fixed point exactly once, in feeRateRawOf. That crossing has a
// rule -- ROUND HALF AWAY FROM ZERO, the rule roundDoubleToI64 implements --
// and the rule needs inputs that can tell it apart from the alternatives,
// which the ladders above cannot: 2.5 and 7.5 bps scale to exact integers, so
// truncating, rounding to nearest and rounding ties to even all agree on them.

// 0.57 bps is stored as a double that scales to 5699.9999999999991, and 2.51
// to 25099.999999999996. Truncating the scaled value drops the last raw off
// both, which is the same defect the funding rate had -- a maker rebate ladder
// quoted in hundredths of a bp would be mispriced on nearly every tier.
TEST(VenueFeeFixedPoint, AFeeRateBecomesARawByRoundingToNearest)
{
  // 2e13 notional (200000 quote units), so the rate raw reads straight off
  // the fee: fee == notional * rateRaw / 1e8 == rateRaw * 200000.
  const Trade t = unitPrint(200'000'00000000LL);
  const auto [maker, taker] = chargedBy(scheduleBps(0.57, 2.51), t);

  EXPECT_EQ(maker, 1'140'000'000LL);  // rate raw 5700, not 5699
  EXPECT_EQ(taker, 5'020'000'000LL);  // rate raw 25100, not 25099
}

// An exact tie, where the rounding rule is the whole answer. 0.00025 bps
// scales to exactly 2.5 and -0.00025 to exactly -2.5: away from zero gives 3
// and -3, truncation gives 2 and -2, and ties-to-even gives 2 and -2 as well.
// The documented rule is away from zero, so a rate and its negation stay
// mirror images -- which is what a fee charged in one direction and rebated
// in the other has to be.
TEST(VenueFeeFixedPoint, AFeeRateTieRoundsAwayFromZero)
{
  const Trade t = unitPrint(200'000'00000000LL);
  const auto [maker, taker] = chargedBy(scheduleBps(0.00025, -0.00025), t);

  EXPECT_EQ(maker, 600'000LL);   // rate raw 3
  EXPECT_EQ(taker, -600'000LL);  // rate raw -3, the exact mirror
}

// The boundary converts a configured rate; it does not police it. A ladder
// above 100% is a configuration mistake, and silently rewriting it into a
// different fee is how the mistake gets charged to an account and never
// noticed. Nothing in this venue refuses such a rate today, so what it does
// is what is pinned: it carries it.
TEST(VenueFeeFixedPoint, AFeeRateAboveOneHundredPercentIsCarriedNotClamped)
{
  const Trade t = unitPrint(200'000'00000000LL);
  const auto [maker, taker] = chargedBy(scheduleBps(/*120%*/ 12'000.0, /*250%*/ 25'000.0), t);

  EXPECT_EQ(maker, 24'000'000'000'000LL);  // 1.2 x the 2e13 notional
  EXPECT_EQ(taker, 50'000'000'000'000LL);  // 2.5 x
}

// The multiply itself truncates toward zero, the way notionalRaw does and the
// way every other money step in this venue does. A notional of
// 20000000003333 at 2.5 bps is 5000000000.83325 raw exactly: a remainder well
// ABOVE half, so truncating and rounding to nearest disagree, and truncation
// is the rule. The rebate side is the mirror: toward zero, not away from it.
TEST(VenueFeeFixedPoint, AFeeTruncatesTowardZeroLikeEveryOtherMoneyStep)
{
  constexpr int64_t kOddNotionalRaw = 20'000'000'003'333LL;  // 200000.00003333 x 1

  const __int128 num = static_cast<__int128>(kOddNotionalRaw) * 25'000;
  ASSERT_GT(static_cast<int64_t>(num % kFeeRateScale) * 2, kFeeRateScale);  // remainder > half

  const Trade t = unitPrint(kOddNotionalRaw);
  const auto [maker, taker] = chargedBy(scheduleBps(2.5, -2.5), t);

  EXPECT_EQ(maker, 5'000'000'000LL);
  EXPECT_EQ(taker, -5'000'000'000LL);
}

// One print, one tier. Resolving a tier is what advances the schedule's
// rolling window, so the two sides of a trade are priced off a single lookup
// and cannot be charged out of different tiers -- including on the two moments
// where a tier could move underneath them: a fill that has just pushed the
// account over a boundary, and a fill that has just aged out of the window.
TEST(VenueFeeFixedPoint, BothSidesOfAPrintArePricedFromOneTier)
{
  const Trade t = unitPrint(200'000'00000000LL);

  flox::FeeSchedule laddered;
  laddered.addTier(0.0, 1.0, 2.0);
  laddered.addTier(1'000'000.0, 3.0, 4.0);
  laddered.recordFill(0, 1'000'000.0);  // exactly onto the upper tier

  const auto [maker, taker] = chargedBy(laddered, t, /*nowRaw*/ 0);
  EXPECT_EQ(maker, 6'000'000'000LL);  // 3 bps, upper tier
  EXPECT_EQ(taker, 8'000'000'000LL);  // 4 bps, the SAME tier

  // The same schedule one nanosecond after that fill leaves the 30-day
  // window: the window is trimmed before either side is priced, so both drop
  // to the lower tier together.
  flox::FeeSchedule aged;
  aged.addTier(0.0, 1.0, 2.0);
  aged.addTier(1'000'000.0, 3.0, 4.0);
  aged.recordFill(0, 1'000'000.0);

  const auto [agedMaker, agedTaker] =
      chargedBy(aged, t, /*nowRaw*/ flox::FeeSchedule::kThirtyDaysNs + 1);
  EXPECT_EQ(agedMaker, 2'000'000'000LL);  // 1 bp, lower tier
  EXPECT_EQ(agedTaker, 4'000'000'000LL);  // 2 bps, the SAME tier
}

// The one property of the pricing that cannot be observed from outside
// engine::Fees: that the tier is looked up ONCE per print rather than once per
// side. Nothing separates the two while flox::FeeSchedule::currentBps is
// idempotent for a fixed timestamp -- evictExpired is the only state it
// touches and the second call has nothing left to evict, and resolveTierIndex
// is a pure read -- so that idempotency is the precondition, and it is
// asserted here rather than assumed. The day currentBps stops being
// idempotent, one lookup and two stop being the same thing, and this is where
// the reason is written down.
//
// needs: template <class Schedule = flox::FeeSchedule> class Fees -- an
// injectable schedule is what would let a test count the lookups directly
// instead of reasoning about what a lookup does.
TEST(VenueFeeFixedPoint, TheTierLookupIsIdempotentForOneTimestamp)
{
  flox::FeeSchedule fs;
  fs.addTier(0.0, 1.0, 2.0);
  fs.addTier(1'000'000.0, 3.0, 4.0);
  fs.recordFill(0, 1'000'000.0);

  const int64_t justExpired = flox::FeeSchedule::kThirtyDaysNs + 1;
  const auto first = fs.currentBps(justExpired);   // this call drops the fill
  const auto second = fs.currentBps(justExpired);  // this one finds nothing to drop

  EXPECT_EQ(first.first, second.first);
  EXPECT_EQ(first.second, second.second);
  EXPECT_EQ(first.first, 1.0);  // the lower tier, the window having been trimmed
  EXPECT_EQ(first.second, 2.0);
}

// ---- what must never become a number --------------------------------------

// A rate that is not a number. It reaches feeRateRawOf from configuration --
// a ladder read out of JSON, a tier computed from a division that had no
// divisor -- and the only safe raw for it is zero: no fee, no rebate, nothing
// moved. What must not happen is a raw at all, because a garbage rate raw is
// spent on a real notional and lands in the ledger as a real transfer.
// roundDoubleToI64 says so in its own words -- a NaN has no nearest
// representable value, and zero is the only answer that cannot be mistaken
// for a real quantity -- and this is the venue holding it to that.
TEST(VenueFeeFixedPoint, ANaNRateChargesNothing)
{
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  ASSERT_NE(kNaN, kNaN);

  EXPECT_EQ(roundDoubleToI64(kNaN), 0);
  EXPECT_EQ(flox::venue::engine::feeRateRawOf(kNaN), 0);

  // Reported: both sides are told the print cost nothing.
  const Trade t = unitPrint(200'000'00000000LL);
  const auto [maker, taker] = chargedOn(scheduleBps(kNaN, kNaN), t, cfg());
  EXPECT_EQ(maker, 0);
  EXPECT_EQ(taker, 0);

  // Settled: not a raw leaves either account, and the venue is not paid.
  Fees f;
  f.setSchedule(scheduleBps(kNaN, kNaN));
  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };

  Ledger led;
  const Amount seed = static_cast<Amount>(1'000'000'000'000'000LL);
  led.deposit(1, QUOTE, seed);
  led.deposit(2, QUOTE, seed);
  f.settle(t, cfg(), 0, led, VENUE_ACCT, sink);

  EXPECT_EQ(led.available(1, QUOTE), seed);
  EXPECT_EQ(led.available(2, QUOTE), seed);
  EXPECT_EQ(led.available(VENUE_ACCT, QUOTE), 0);
}

// ---- the symbol's own two scales ------------------------------------------

// A fee prices the SYMBOL's notional, at the symbol's scales, the same
// arithmetic the reservations and the margin use. A symbol carries two of
// them and they are equal only by default: the price raw is read against
// priceScale and the quantity raw against qtyScale, each in its own place.
//
// The same economic print -- one unit at 200000 -- costs the same whichever
// way the symbol counts, and that is the point: the raws change, the money
// does not.
TEST(VenueFeeFixedPoint, TheNotionalReadsEachScaleInItsOwnPlace)
{
  constexpr int64_t kFeeRaw = 5'000'000'000LL;  // 2.5 bps of 200000 quote units

  SymbolConfig plain = cfg();  // 1e8 / 1e8
  const auto [plainMaker, plainTaker] =
      chargedOn(scheduleBps(2.5, 2.5), unitPrint(200'000'00000000LL), plain);
  EXPECT_EQ(plainMaker, kFeeRaw);
  EXPECT_EQ(plainTaker, kFeeRaw);

  // Quantities counted in thousandths: the quantity raw is a thousand times
  // smaller, the price raw is untouched.
  SymbolConfig coarseLots = cfg();
  coarseLots.qtyScale = 1000;
  Trade lots = unitPrint(200'000'00000000LL);
  lots.quantity = Quantity::fromRaw(1000);
  const auto [lotsMaker, lotsTaker] = chargedOn(scheduleBps(2.5, 2.5), lots, coarseLots);
  EXPECT_EQ(lotsMaker, kFeeRaw);
  EXPECT_EQ(lotsTaker, kFeeRaw);

  // And the other way round: a symbol quoted in ten-thousandths, counted in
  // hundred-millionths.
  SymbolConfig coarseTicks = cfg();
  coarseTicks.priceScale = 10'000;
  Trade ticks = unitPrint(200'000'0000LL);
  const auto [ticksMaker, ticksTaker] = chargedOn(scheduleBps(2.5, 2.5), ticks, coarseTicks);
  EXPECT_EQ(ticksMaker, kFeeRaw);
  EXPECT_EQ(ticksTaker, kFeeRaw);
}

// The two scales taken apart where the arithmetic can tell them apart at all.
// notionalRaw is price x quantity x kMoneyScale / (priceScale x qtyScale),
// which is symmetric in the two scales for every input that does not reach
// its overflow guard -- so on an ordinary symbol, reading them in the wrong
// order is invisible. Past the guard it stops being symmetric, because the
// guard divides by priceScale FIRST to keep the product inside the 128-bit
// Amount, and which scale is divided first is then the whole answer.
//
// The instrument: quoted to sixteen decimals, counted in whole units. A
// trillion units at 0.2 is 2e22 quote units of notional -- exactly the region
// ledger.h documents as the guard's, and the only region where a fee can say
// which scale it read where.
TEST(VenueFeeFixedPoint, TheNotionalReadsTheScalesInTheOrderLedgerDefines)
{
  SymbolConfig wide = cfg();
  wide.priceScale = 10'000'000'000'000'000LL;  // 1e16
  wide.qtyScale = 1;                           // whole units
  ASSERT_TRUE(scalesValid(wide.priceScale, wide.qtyScale));

  Trade t = print();
  t.price = Price::fromRaw(2'000'000'000'000'001LL);
  t.quantity = Quantity::fromRaw(1'000'000'000'000'003LL);

  // What ledger.h makes of it, with the arguments in the order it defines.
  const Amount notional = notionalRaw(t.price.raw(), t.quantity.raw(), wide.priceScale,
                                      wide.qtyScale);
  const Amount swapped = notionalRaw(t.price.raw(), t.quantity.raw(), wide.qtyScale,
                                     wide.priceScale);
  ASSERT_NE(notional, swapped);  // past the guard the order is observable

  const auto [maker, taker] = chargedOn(scheduleBps(2.5, 2.5), t, wide);
  EXPECT_EQ(maker, 5'000'000'000'000'000'000LL);  // 2.5 bps of 2e22
  EXPECT_EQ(taker, 5'000'000'000'000'000'000LL);
  // Read the other way round the same print costs 5000000000000017500.
  EXPECT_NE(maker, static_cast<int64_t>(rateOnNotional(swapped, 25'000, kFeeRateScale)));
}
