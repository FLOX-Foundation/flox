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
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::engine::Fees;

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
