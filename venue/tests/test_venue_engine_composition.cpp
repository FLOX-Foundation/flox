/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The last four pieces to leave the template, tested away from it:
 * engine::Fees, engine::OcoBook, engine::Integrity and engine::QuoteLegs.
 *
 * Each of them used to be loose members and loose methods of
 * MatchingEngine<Book>, which meant the only way to ask any of them a
 * question was to build a venue, feed it order flow and read the answer off
 * the event stream. Three of the four had no test of their own at all --
 * their behaviour was covered exactly as far as some other test happened to
 * walk over it, and the fee arithmetic was written out three times with
 * nothing comparing the three.
 *
 * So the point of this file is the direct question. The engine-level half at
 * the bottom is deliberately small: it checks the composition -- that the
 * engine still asks these components and still publishes their answers --
 * and leaves the behaviour to the cases above it, where a wrong answer names
 * itself instead of arriving as a missing event twelve calls later.
 */
#include "flox-venue/engine/fees.h"
#include "flox-venue/engine/integrity.h"
#include "flox-venue/engine/oco.h"
#include "flox-venue/engine/quote.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::engine::Fees;
using flox::venue::engine::Integrity;
using flox::venue::engine::OcoBook;
using flox::venue::engine::QuoteLegs;

namespace
{

constexpr SymbolId SYM = 7;
constexpr AssetId BASE = 1;
constexpr AssetId QUOTE = 2;
constexpr uint64_t VENUE_ACCT = 999;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(10000.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  return c;
}

NewOrder limitOrder(OrderId id, Side side, double price, double quantity, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = px(price);
  o.quantity = qty(quantity);
  o.accountId = acct;
  return o;
}

Trade print(double price, double quantity, uint64_t makerAcct = 1, uint64_t takerAcct = 2)
{
  Trade t;
  t.symbol = SYM;
  t.price = px(price);
  t.quantity = qty(quantity);
  t.makerId = 10;
  t.takerId = 20;
  t.makerAccount = makerAcct;
  t.takerAccount = takerAcct;
  t.takerSide = Side::BUY;
  t.tradeId = 1;
  return t;
}

// A flat schedule: one tier, from the beginning of time, maker and taker
// priced independently so a test that swaps the two is a failing test.
flox::FeeSchedule schedule(double makerBps, double takerBps)
{
  flox::FeeSchedule fs;
  fs.addTier(0.0, makerBps, takerBps);
  return fs;
}

// ---- engine::Fees --------------------------------------------------------

TEST(VenueEngineFees, UnconfiguredChargesNothing)
{
  Fees f;
  EXPECT_FALSE(f.enabled());

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };
  const SymbolConfig c = cfg();

  f.emit(print(100.0, 1.0), c, 0, sink);
  EXPECT_TRUE(out.empty());

  Ledger led;
  led.deposit(1, QUOTE, 1000);
  f.settle(print(100.0, 1.0), c, 0, led, VENUE_ACCT, sink);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(led.available(1, QUOTE), 1000);  // not a cent moved
  EXPECT_EQ(led.available(VENUE_ACCT, QUOTE), 0);
}

// The report path: both sides are told what the print cost, maker first, and
// nothing moves because there is nothing to move it through.
TEST(VenueEngineFees, ReportsNameBothSidesMakerFirst)
{
  Fees f;
  f.setSchedule(schedule(1.0, 5.0));
  EXPECT_TRUE(f.enabled());

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };

  f.emit(print(100.0, 2.0), cfg(), 0, sink);

  ASSERT_EQ(out.size(), 2U);
  const auto* maker = std::get_if<FeeCharged>(&out[0]);
  const auto* taker = std::get_if<FeeCharged>(&out[1]);
  ASSERT_NE(maker, nullptr);
  ASSERT_NE(taker, nullptr);
  EXPECT_TRUE(maker->maker);
  EXPECT_FALSE(taker->maker);
  EXPECT_EQ(maker->id, 10U);
  EXPECT_EQ(taker->id, 20U);
  EXPECT_EQ(maker->account, 1U);
  EXPECT_EQ(taker->account, 2U);
  EXPECT_EQ(maker->symbol, SYM);
  // 200 notional at 1bp / 5bp.
  EXPECT_NEAR(maker->fee.toDouble(), 0.02, 1e-9);
  EXPECT_NEAR(taker->fee.toDouble(), 0.10, 1e-9);
}

// The settlement path: the same two reports, and the money behind them. Value
// is conserved -- what leaves the participants arrives at the venue.
TEST(VenueEngineFees, SettlementMovesTheMoneyItReports)
{
  Fees f;
  f.setSchedule(schedule(1.0, 5.0));

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };

  Ledger led;
  led.deposit(1, QUOTE, 100000000);
  led.deposit(2, QUOTE, 100000000);
  const Amount before = led.available(1, QUOTE) + led.available(2, QUOTE) +
                        led.available(VENUE_ACCT, QUOTE);

  f.settle(print(100.0, 2.0), cfg(), 0, led, VENUE_ACCT, sink);

  ASSERT_EQ(out.size(), 2U);
  const auto* maker = std::get_if<FeeCharged>(&out[0]);
  const auto* taker = std::get_if<FeeCharged>(&out[1]);
  ASSERT_NE(maker, nullptr);
  ASSERT_NE(taker, nullptr);

  const Amount makerFee = static_cast<Amount>(maker->fee.raw());
  const Amount takerFee = static_cast<Amount>(taker->fee.raw());
  EXPECT_EQ(led.available(1, QUOTE), 100000000 - makerFee);
  EXPECT_EQ(led.available(2, QUOTE), 100000000 - takerFee);
  EXPECT_EQ(led.available(VENUE_ACCT, QUOTE), makerFee + takerFee);
  EXPECT_EQ(led.available(1, QUOTE) + led.available(2, QUOTE) +
                led.available(VENUE_ACCT, QUOTE),
            before);
}

// A rebate is a negative fee and goes the other way, out of the venue's
// account and into the maker's. Same code, same conservation.
TEST(VenueEngineFees, ARebateRunsBackwardsAndStillConserves)
{
  Fees f;
  f.setSchedule(schedule(-2.0, 5.0));

  std::vector<OutboundEvent> out;
  EventSink sink = [&out](const OutboundEvent& e)
  { out.push_back(e); };

  Ledger led;
  led.deposit(1, QUOTE, 100000000);
  led.deposit(2, QUOTE, 100000000);
  led.deposit(VENUE_ACCT, QUOTE, 100000000);
  const Amount before = led.available(1, QUOTE) + led.available(2, QUOTE) +
                        led.available(VENUE_ACCT, QUOTE);

  f.settle(print(100.0, 2.0), cfg(), 0, led, VENUE_ACCT, sink);

  EXPECT_GT(led.available(1, QUOTE), 100000000);  // the maker was paid
  EXPECT_LT(led.available(2, QUOTE), 100000000);  // the taker paid
  EXPECT_EQ(led.available(1, QUOTE) + led.available(2, QUOTE) +
                led.available(VENUE_ACCT, QUOTE),
            before);
}

// The notional a fee is priced against is the SYMBOL's notional, the same
// arithmetic reservations and margin use. This was written out three times
// before the component existed; a symbol that declares its own scales is what
// tells the three apart, so it is asked here directly.
TEST(VenueEngineFees, ThePriceIsTheSymbolsNotionalNotTheDefaultScales)
{
  std::vector<OutboundEvent> a;
  std::vector<OutboundEvent> b;
  EventSink sinkA = [&a](const OutboundEvent& e)
  { a.push_back(e); };
  EventSink sinkB = [&b](const OutboundEvent& e)
  { b.push_back(e); };

  Fees f1;
  f1.setSchedule(schedule(1.0, 5.0));
  f1.emit(print(100.0, 2.0), cfg(), 0, sinkA);

  // The same economic print on a symbol whose quantities are counted in
  // thousandths rather than hundred-millionths: half the raw numbers change,
  // the notional does not, so neither may the fee.
  SymbolConfig coarse = cfg();
  coarse.qtyScale = 1000;
  Trade t = print(100.0, 2.0);
  t.quantity = Quantity::fromRaw(2 * 1000);

  Fees f2;
  f2.setSchedule(schedule(1.0, 5.0));
  f2.emit(t, coarse, 0, sinkB);

  ASSERT_EQ(a.size(), 2U);
  ASSERT_EQ(b.size(), 2U);
  EXPECT_EQ(std::get<FeeCharged>(a[0]).fee.raw(), std::get<FeeCharged>(b[0]).fee.raw());
  EXPECT_EQ(std::get<FeeCharged>(a[1]).fee.raw(), std::get<FeeCharged>(b[1]).fee.raw());
}

// ---- engine::OcoBook -----------------------------------------------------

TEST(VenueEngineOco, MembershipIsKeptInBothDirections)
{
  OcoBook o;
  EXPECT_TRUE(o.empty());
  EXPECT_EQ(o.groupOf(1), 0U);

  o.link(1, 42);
  o.link(2, 42);
  EXPECT_FALSE(o.empty());
  EXPECT_EQ(o.groupOf(1), 42U);
  EXPECT_EQ(o.groupOf(2), 42U);
  EXPECT_EQ(o.groupOf(3), 0U);

  o.unlink(1);
  EXPECT_EQ(o.groupOf(1), 0U);
  EXPECT_EQ(o.groupOf(2), 42U);
  o.unlink(2);
  EXPECT_TRUE(o.empty());
}

TEST(VenueEngineOco, UnlinkingSomethingUngroupedIsANoOp)
{
  OcoBook o;
  o.unlink(1);  // nothing linked at all
  o.link(1, 42);
  o.unlink(1);
  o.unlink(1);  // and again, after it already left
  EXPECT_TRUE(o.empty());
}

// The reason unlink() exists. A leg that leaves by any door but stays in the
// group vector cancels whatever order later REUSES its id -- an id the venue
// is entitled to reuse, because the first order is gone.
TEST(VenueEngineOco, ADepartedLegCannotCancelAReusedId)
{
  OcoBook o;
  o.link(10, 1);
  o.link(11, 1);
  o.unlink(10);  // id 10 leaves the venue

  o.link(10, 2);  // id 10 is reused, by a different group

  std::vector<OrderId> losers;
  o.noteFill(11, 0);  // group 1 is decided by its other member
  o.drainLosers(losers);
  EXPECT_TRUE(losers.empty());  // the reused id-10 is NOT in group 1 any more
  EXPECT_EQ(o.groupOf(10), 2U);
}

TEST(VenueEngineOco, TheWinnerSurvivesAndTheSiblingsComeOutIdSorted)
{
  OcoBook o;
  // Linked out of order on purpose: the verdict must not depend on it.
  o.link(30, 1);
  o.link(10, 1);
  o.link(20, 1);

  EXPECT_TRUE(o.nothingPending());
  o.noteFill(20, 0);  // id 20 filled as the maker
  EXPECT_FALSE(o.nothingPending());

  std::vector<OrderId> losers;
  o.drainLosers(losers);
  EXPECT_EQ(losers, (std::vector<OrderId>{10, 30}));
  EXPECT_TRUE(o.nothingPending());
  EXPECT_TRUE(o.empty());  // the whole group is dissolved, winner included
}

TEST(VenueEngineOco, EitherSideOfAPrintWinsItsOwnGroup)
{
  OcoBook o;
  o.link(1, 100);
  o.link(2, 100);
  o.link(3, 200);
  o.link(4, 200);

  o.noteFill(/*maker=*/1, /*taker=*/3);

  std::vector<OrderId> losers;
  o.drainLosers(losers);
  // Group 100 first (the maker's, collected first), then group 200.
  EXPECT_EQ(losers, (std::vector<OrderId>{2, 4}));
}

TEST(VenueEngineOco, AGroupDecidedTwiceInOneSubmitResolvesOnce)
{
  OcoBook o;
  o.link(1, 100);
  o.link(2, 100);
  o.link(3, 100);

  o.noteFill(1, 2);  // both legs of the same group printed against each other

  std::vector<OrderId> losers;
  o.drainLosers(losers);
  // The first verdict dissolves the group: id 1 won it, so 2 and 3 lose. The
  // second entry finds no group left and adds nothing -- id 2 is named once.
  EXPECT_EQ(losers, (std::vector<OrderId>{2, 3}));
}

TEST(VenueEngineOco, DrainingClearsTheCallersBuffer)
{
  OcoBook o;
  o.link(1, 5);
  o.link(2, 5);
  o.noteFill(1, 0);

  std::vector<OrderId> losers{777};  // stale content from a previous sweep
  o.drainLosers(losers);
  EXPECT_EQ(losers, (std::vector<OrderId>{2}));
}

// ---- engine::Integrity ---------------------------------------------------

// Both counters are written with a report, and the report is the only place
// an operator ever sees the event -- so the text is part of the behaviour,
// not decoration. Captured off stderr rather than trusted, through gtest's
// own redirection: this file is compiled into the engine-only build, which is
// the one that has to exist where POSIX does not, so it reaches for no
// platform header of its own (scripts/check_engine_portability.py).
std::string captureStderr(void (*body)(Integrity&), Integrity& i)
{
#if GTEST_HAS_STREAM_REDIRECTION
  ::testing::internal::CaptureStderr();
  body(i);
  std::fflush(stderr);
  return ::testing::internal::GetCapturedStderr();
#else
  body(i);
  return std::string();
#endif
}

// True where the text above was actually captured; where it was not, the
// counters are still checked and the text assertions are skipped rather than
// passing on an empty string.
constexpr bool kStderrCaptured = GTEST_HAS_STREAM_REDIRECTION != 0;

TEST(VenueEngineIntegrity, BothCountersStartAtZeroAndCountWhatTheyName)
{
  Integrity i;
  EXPECT_EQ(i.unsettledTrades(), 0U);
  EXPECT_EQ(i.droppedSnapshotRecords(), 0U);

  const std::string dropped = captureStderr([](Integrity& x)
                                            {
                                              x.dropSnapshotRecord(17);
                                              x.dropSnapshotRecord(18); },
                                            i);
  EXPECT_EQ(i.droppedSnapshotRecords(), 2U);
  EXPECT_EQ(i.unsettledTrades(), 0U);  // the two counters are not one counter
  if (kStderrCaptured)
  {
    EXPECT_NE(dropped.find("dropped snapshot-only record (tag 17)"), std::string::npos);
    EXPECT_NE(dropped.find("(tag 18)"), std::string::npos);
  }

  const std::string unsettled = captureStderr([](Integrity& x)
                                              { x.reportUnsettled(4242, SYM, "why", 8); },
                                              i);
  EXPECT_EQ(i.unsettledTrades(), 1U);
  EXPECT_EQ(i.droppedSnapshotRecords(), 2U);
  if (kStderrCaptured)
  {
    EXPECT_NE(unsettled.find("trade 4242"), std::string::npos);
    EXPECT_NE(unsettled.find("symbol 7"), std::string::npos);
    EXPECT_NE(unsettled.find("(why, account 8)"), std::string::npos);
    EXPECT_NE(unsettled.find("no value moved"), std::string::npos);
  }
}

// ---- engine::QuoteLegs ---------------------------------------------------

Quote twoSided()
{
  Quote q;
  q.bidId = 1;
  q.askId = 2;
  q.symbol = SYM;
  q.bidPrice = px(99.0);
  q.bidQty = qty(3.0);
  q.askPrice = px(101.0);
  q.askQty = qty(4.0);
  q.accountId = 77;
  q.stp = STPMode::CancelNewest;
  q.lastLook = true;
  q.postOnly = true;
  q.reduceOnly = true;
  q.tif = TimeInForce::GTD;
  q.visibleQuantity = qty(1.0);
  q.expiryNs = SeqNanos::fromRaw(123456);
  q.clientOrderId = 555;
  return q;
}

// Every field a quote carries is a field its legs carry. Asked field by
// field, on BOTH legs, because the failure this guards against is exactly a
// field that reaches one side and not the other.
TEST(VenueEngineQuoteLegs, EveryFieldOfTheQuoteReachesBothLegs)
{
  const Quote q = twoSided();
  std::array<NewOrder, 2> legs;
  ASSERT_EQ(QuoteLegs::build(q, SYM, legs), 2U);

  const NewOrder& bid = legs[0];
  const NewOrder& ask = legs[1];

  EXPECT_EQ(bid.side, Side::BUY);
  EXPECT_EQ(ask.side, Side::SELL);
  EXPECT_EQ(bid.id, q.bidId);
  EXPECT_EQ(ask.id, q.askId);
  EXPECT_EQ(bid.price.raw(), q.bidPrice.raw());
  EXPECT_EQ(ask.price.raw(), q.askPrice.raw());
  EXPECT_EQ(bid.quantity.raw(), q.bidQty.raw());
  EXPECT_EQ(ask.quantity.raw(), q.askQty.raw());

  for (const NewOrder& leg : {bid, ask})
  {
    EXPECT_EQ(leg.symbol, SYM);
    EXPECT_EQ(leg.type, OrderType::LIMIT);
    EXPECT_EQ(leg.accountId, q.accountId);
    EXPECT_EQ(leg.stp, q.stp);
    EXPECT_EQ(leg.lastLook, q.lastLook);
    EXPECT_EQ(leg.postOnly, q.postOnly);
    EXPECT_EQ(leg.reduceOnly, q.reduceOnly);
    EXPECT_EQ(leg.tif, q.tif);
    EXPECT_EQ(leg.visibleQuantity.raw(), q.visibleQuantity.raw());
    EXPECT_EQ(leg.expiryNs.raw(), q.expiryNs.raw());
    // The name the submitter gave the QUOTE -- both legs carry it, so their
    // reports can be joined back to each other.
    EXPECT_EQ(leg.clientOrderId, q.clientOrderId);
  }
}

TEST(VenueEngineQuoteLegs, ASideQuotedAtZeroIsNotALeg)
{
  Quote q = twoSided();
  std::array<NewOrder, 2> legs;

  q.bidQty = qty(0.0);
  ASSERT_EQ(QuoteLegs::build(q, SYM, legs), 1U);
  EXPECT_EQ(legs[0].side, Side::SELL);
  EXPECT_EQ(legs[0].id, q.askId);

  q = twoSided();
  q.askQty = qty(0.0);
  ASSERT_EQ(QuoteLegs::build(q, SYM, legs), 1U);
  EXPECT_EQ(legs[0].side, Side::BUY);
  EXPECT_EQ(legs[0].id, q.bidId);

  q.bidQty = qty(0.0);
  EXPECT_EQ(QuoteLegs::build(q, SYM, legs), 0U);
}

// The symbol comes from the ENGINE, not from the quote: a quote is answered
// by the engine that owns the instrument it named, and the leg is stamped
// with that instrument.
TEST(VenueEngineQuoteLegs, TheSymbolIsTheOneHandedIn)
{
  Quote q = twoSided();
  q.symbol = 4242;
  std::array<NewOrder, 2> legs;
  ASSERT_EQ(QuoteLegs::build(q, SYM, legs), 2U);
  EXPECT_EQ(legs[0].symbol, SYM);
  EXPECT_EQ(legs[1].symbol, SYM);
}

// ---- the composition -----------------------------------------------------
//
// The engine's half: it still asks these components, and still publishes
// their answers where it used to publish its own.

TEST(VenueEngineComposition, ASnapshotRecordInLiveTrafficIsCountedByTheEngine)
{
  MatchingEngine<MatchingBook> e(cfg(), [](const OutboundEvent&) {});
  EXPECT_EQ(e.droppedSnapshotRecords(), 0U);

  e.submit(InboundCommand{RestoreBalance{}}, 1);
  EXPECT_EQ(e.droppedSnapshotRecords(), 1U);
  EXPECT_EQ(e.unsettledTrades(), 0U);  // a dropped record settled nothing
}

TEST(VenueEngineComposition, TheLosingLegOfAnOcoGroupIsCanceledByTheEngine)
{
  std::vector<OutboundEvent> out;
  MatchingEngine<MatchingBook> e(cfg(), [&out](const OutboundEvent& ev)
                                 { out.push_back(ev); });

  NewOrder a = limitOrder(1, Side::BUY, 99.0, 1.0, 1);
  a.ocoGroup = 7;
  NewOrder b = limitOrder(2, Side::BUY, 98.0, 1.0, 1);
  b.ocoGroup = 7;
  e.submit(InboundCommand{a}, 1);
  e.submit(InboundCommand{b}, 2);
  EXPECT_EQ(e.restingOrderCount(), 2U);

  out.clear();
  e.submit(InboundCommand{limitOrder(3, Side::SELL, 99.0, 1.0, 2)}, 3);

  // id 1 filled and won the group; id 2 lost it and was canceled for that
  // reason, and no other order was touched.
  EXPECT_EQ(e.restingOrderCount(), 0U);
  int canceled = 0;
  for (const OutboundEvent& ev : out)
  {
    if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      EXPECT_EQ(c->id, 2U);
      EXPECT_EQ(c->reason, CancelReason::OcoTriggered);
      ++canceled;
    }
  }
  EXPECT_EQ(canceled, 1);
}

TEST(VenueEngineComposition, AQuoteBecomesItsTwoLegsThroughTheEngine)
{
  std::vector<OutboundEvent> out;
  MatchingEngine<MatchingBook> e(cfg(), [&out](const OutboundEvent& ev)
                                 { out.push_back(ev); });

  Quote q = twoSided();
  q.lastLook = false;    // the venue has no last-look window configured
  q.reduceOnly = false;  // spot: nothing to reduce
  q.tif = TimeInForce::GTC;
  q.expiryNs = SeqNanos{};
  e.submit(InboundCommand{q}, 1);

  EXPECT_EQ(e.restingOrderCount(), 2U);
  std::vector<OrderId> accepted;
  for (const OutboundEvent& ev : out)
  {
    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      accepted.push_back(a->id);
      EXPECT_EQ(a->clientOrderId, q.clientOrderId);  // both legs, one name
      EXPECT_EQ(a->account, q.accountId);
    }
  }
  // The bid reaches the book first, exactly as the component orders them.
  EXPECT_EQ(accepted, (std::vector<OrderId>{q.bidId, q.askId}));
}

TEST(VenueEngineComposition, TheEngineChargesThroughTheComponentItOwns)
{
  std::vector<OutboundEvent> out;
  Ledger led;
  MatchingEngine<MatchingBook> e(cfg(), [&out](const OutboundEvent& ev)
                                 { out.push_back(ev); });
  e.setLedger(&led, VENUE_ACCT);
  led.deposit(1, BASE, 100000000000LL);
  led.deposit(2, QUOTE, 100000000000LL);
  e.setFeeSchedule(schedule(1.0, 5.0));

  e.submit(InboundCommand{limitOrder(1, Side::SELL, 100.0, 1.0, 1)}, 1);
  e.submit(InboundCommand{limitOrder(2, Side::BUY, 100.0, 1.0, 2)}, 2);

  int fees = 0;
  for (const OutboundEvent& ev : out)
  {
    if (std::get_if<FeeCharged>(&ev) != nullptr)
    {
      ++fees;
    }
  }
  EXPECT_EQ(fees, 2);
  EXPECT_GT(led.available(VENUE_ACCT, QUOTE), 0);  // the venue was paid
}

}  // namespace
