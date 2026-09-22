/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * T058: FixCodec encoded OrderCanceled without tag 151 (LeavesQty). A
 * counterparty that reads LeavesQty off a terminal report (routine practice
 * for an IOC/FOK residual) saw an order it never fully filled as fully
 * filled -- the unfilled remainder vanished from its own accounting.
 *
 * The fix carries leavesQty/cumQty on OrderCanceled (both) and OrderRejected
 * (cumQty only -- a rejected order is never left resting, so its LeavesQty
 * is always 0 by construction) and writes FIX 151/14 from them on every
 * report. The venue's two matching-book implementations (MatchingBook here,
 * flox::LadderBook in production) both grew a RestingOrder::cumQty running
 * total so a plain cancel of a previously-partially-filled resting order
 * reports the real cumulative fill, not 0.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sbe_order_entry_codec.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.lotSize = qty(0.001);
  c.minQty = qty(0.001);
  return c;
}

// basePriceRaw 0, tick 0.01, enough levels to cover [0, 1000] with maxOrders
// generous for these small scenarios.
LadderBook::Config lc() { return LadderBook::Config{0, px(0.01).raw(), 100000, 1024}; }

NewOrder order(OrderId id, Side side, double price, double quantity, uint64_t account = 1,
               TimeInForce tif = TimeInForce::GTC, OrderType type = OrderType::LIMIT)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = side;
  o.type = type;
  o.price = px(price);
  o.quantity = qty(quantity);
  o.accountId = account;
  o.tif = tif;
  return o;
}

// One FIX tag out of an encoded message, or "" when the tag is absent.
std::string tag(const std::string& msg, int t)
{
  const std::string key = std::to_string(t) + "=";
  size_t pos = 0;
  while ((pos = msg.find(key, pos)) != std::string::npos)
  {
    const bool atStart = pos == 0 || msg[pos - 1] == '\x01';
    if (atStart)
    {
      const size_t end = msg.find('\x01', pos);
      return msg.substr(pos + key.size(), end - pos - key.size());
    }
    pos += key.size();
  }
  return {};
}

struct Capture
{
  std::vector<OutboundEvent> ev;
  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }
  template <class T>
  const T* firstWhere(bool (*pred)(const T&)) const
  {
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<T>(&e); x != nullptr && pred(*x))
      {
        return x;
      }
    }
    return nullptr;
  }
  template <class T>
  const T* first() const
  {
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<T>(&e))
      {
        return x;
      }
    }
    return nullptr;
  }
};

uint64_t rootU64(const std::vector<uint8_t>& f, size_t off)
{
  return sbe::getU64(f.data() + sbe::kHeaderSize + off);
}

}  // namespace

// ---- FixCodec: pure function of the event, no engine needed ---------------

TEST(LeavesCumQty, FixCodecWritesBothTagsFromTheEventOnOrderCanceled)
{
  OrderCanceled c;
  c.id = 7;
  c.symbol = SYM;
  c.reason = CancelReason::ImmediateOrCancelResidual;
  c.account = 1;
  c.clientOrderId = 42;
  c.leavesQty = qty(3);
  c.cumQty = qty(2);

  const std::string wire = FixCodec::encode(OutboundEvent{c});
  EXPECT_EQ(tag(wire, 150), "4") << "ExecType Canceled";
  EXPECT_EQ(tag(wire, 39), "4") << "OrdStatus Canceled";
  EXPECT_EQ(tag(wire, 151), "3") << "LeavesQty: the residual this cancel killed";
  EXPECT_EQ(tag(wire, 14), "2") << "CumQty: what the order filled before this cancel";
}

TEST(LeavesCumQty, FixCodecWritesZerosWhenTheOrderNeverFilled)
{
  OrderCanceled c;
  c.id = 7;
  c.symbol = SYM;
  c.reason = CancelReason::UserRequested;
  c.account = 1;
  c.leavesQty = qty(5);
  c.cumQty = qty(0);

  const std::string wire = FixCodec::encode(OutboundEvent{c});
  EXPECT_EQ(tag(wire, 151), "5");
  EXPECT_EQ(tag(wire, 14), "0") << "14 is present and explicitly 0, not omitted";
}

TEST(LeavesCumQty, FixCodecWritesCumQtyOnOrderRejectedAndLeavesQtyIsAlwaysZero)
{
  OrderRejected j;
  j.id = 9;
  j.symbol = SYM;
  j.reason = RejectReason::InsufficientFunds;
  j.account = 1;
  j.cumQty = qty(0);  // every call site in this engine reports 0 here today

  const std::string wire = FixCodec::encode(OutboundEvent{j});
  EXPECT_EQ(tag(wire, 150), "8") << "ExecType Rejected";
  EXPECT_EQ(tag(wire, 39), "8") << "OrdStatus Rejected";
  EXPECT_EQ(tag(wire, 151), "0") << "a rejected order is never left resting";
  EXPECT_EQ(tag(wire, 14), "0");
}

// ---- SBE: same two tags, schema v9 -----------------------------------------

TEST(LeavesCumQty, SbeCodecWritesBothQuantitiesOnOrderCanceledAfterClOrdId)
{
  OrderCanceled c;
  c.id = 7;
  c.symbol = SYM;
  c.reason = CancelReason::ImmediateOrCancelResidual;
  c.account = 1;
  c.clientOrderId = 42;
  c.leavesQty = qty(3);
  c.cumQty = qty(2);

  std::vector<uint8_t> f;
  SbeOrderEntryCodec::encode(OutboundEvent{c}, f, /*seq=*/5);
  ASSERT_FALSE(f.empty());

  const sbe::Header h = sbe::readHeader(f.data());
  EXPECT_EQ(h.version, SbeOrderEntryCodec::kVersion);
  EXPECT_EQ(h.blockLength, SbeOrderEntryCodec::kBlockCanceled);

  // orderId(8) symbol(4) reason(1) seq(8) clOrdId(8) leavesQty(8) cumQty(8)
  EXPECT_EQ(rootU64(f, 21), 42u) << "clOrdId keeps its version-4 offset";
  EXPECT_EQ(static_cast<int64_t>(rootU64(f, 29)), qty(3).raw()) << "leavesQty, appended after clOrdId";
  EXPECT_EQ(static_cast<int64_t>(rootU64(f, 37)), qty(2).raw()) << "cumQty, appended after leavesQty";

  // seqOf must still find `seq` correctly now that two more fields trail it
  // (T058 grew the trailing width seqOffsetIn subtracts for this template).
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(f.data(), f.size()), 5u);
}

TEST(LeavesCumQty, SbeCodecWritesCumQtyOnOrderRejectedAfterClOrdId)
{
  OrderRejected j;
  j.id = 9;
  j.symbol = SYM;
  j.reason = RejectReason::InsufficientFunds;
  j.account = 1;
  j.clientOrderId = 11;
  j.cumQty = qty(0);

  std::vector<uint8_t> f;
  SbeOrderEntryCodec::encode(OutboundEvent{j}, f, /*seq=*/9);
  ASSERT_FALSE(f.empty());

  const sbe::Header h = sbe::readHeader(f.data());
  EXPECT_EQ(h.blockLength, SbeOrderEntryCodec::kBlockRejected);

  // orderId(8) symbol(4) reason(1) seq(8) clOrdId(8) cumQty(8)
  EXPECT_EQ(rootU64(f, 21), 11u) << "clOrdId keeps its version-4 offset";
  EXPECT_EQ(static_cast<int64_t>(rootU64(f, 29)), 0) << "cumQty, appended after clOrdId";

  EXPECT_EQ(SbeOrderEntryCodec::seqOf(f.data(), f.size()), 9u)
      << "Rejected's trailing width grew by 8 (cumQty only, not 16 like Canceled)";
}

// A v8 frame (pre-T058) has neither field, and a version-gated reader must
// still find `seq` at its old offset -- the backward-compatibility half of
// appending at the end of the block. Synthesized by hand: kVersion is now 9,
// so there is no live encoder for a v8 frame to reuse.
TEST(LeavesCumQty, SeqOffsetInStillDecodesAPreT058VersionEightFrame)
{
  std::vector<uint8_t> f;
  sbe::putHeader(f, /*blockLength=*/29,
                 static_cast<uint16_t>(SbeOrderEntryCodec::OutTmpl::Canceled),
                 SbeOrderEntryCodec::kSchemaId, /*version=*/8);
  sbe::putU64(f, 7);    // orderId
  sbe::putU32(f, SYM);  // symbol
  sbe::putU8(f, 0);     // reason
  sbe::putU64(f, 123);  // seq
  sbe::putU64(f, 42);   // clOrdId
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(f.data(), f.size()), 123u);
}

// ---- Engine integration: the exact bug scenario ----------------------------
// A resting maker for less than the incoming IOC size: the taker partially
// fills, and the unfilled remainder is canceled rather than rested. Before
// the fix, the FIX report for that cancel carried no 151/14 at all, so a
// counterparty that reads LeavesQty off terminal reports saw the order as
// fully filled.

TEST(LeavesCumQty, IocResidualCancelReports151AsTheUnfilledRemainderAnd14AsWhatFilled)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 2)}, 1);  // resting maker, qty 2
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 5, 1, TimeInForce::IOC)},
             2);  // IOC taker, qty 5 -- fills 2, kills 3

  const auto* canceled = cap.first<OrderCanceled>();
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->id, 200u);
  EXPECT_EQ(canceled->reason, CancelReason::ImmediateOrCancelResidual);
  EXPECT_EQ(canceled->leavesQty, qty(3)) << "5 requested - 2 filled = 3 killed";
  EXPECT_EQ(canceled->cumQty, qty(2)) << "2 filled before the residual was killed";

  const std::string wire = FixCodec::encode(OutboundEvent{*canceled});
  EXPECT_EQ(tag(wire, 151), "3");
  EXPECT_EQ(tag(wire, 14), "2");

  std::vector<uint8_t> sbeFrame;
  SbeOrderEntryCodec::encode(OutboundEvent{*canceled}, sbeFrame, /*seq=*/1);
  EXPECT_EQ(static_cast<int64_t>(rootU64(sbeFrame, 29)), qty(3).raw());
  EXPECT_EQ(static_cast<int64_t>(rootU64(sbeFrame, 37)), qty(2).raw());
}

// A resting order that already filled part of its size on an earlier cross,
// then is plainly canceled by its owner: 151 must be the CURRENT remainder
// (not the original order size) and 14 must be the running total from
// RestingOrder::cumQty, not 0.
TEST(LeavesCumQty, PlainCancelOfAPartiallyFilledRestingOrderReportsTheRunningCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 5)}, 1);  // resting maker, qty 5
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 2)}, 2);   // GTC taker fills 2, rests 0 left over

  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 3);

  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 100; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(3)) << "5 - 2 already filled = 3 still resting at cancel time";
  EXPECT_EQ(canceled->cumQty, qty(2)) << "the 2 that filled before this cancel";

  const std::string wire = FixCodec::encode(OutboundEvent{*canceled});
  EXPECT_EQ(tag(wire, 151), "3");
  EXPECT_EQ(tag(wire, 14), "2");
}

// A resting order canceled having never traded: 151 is its full size, 14 is
// 0 -- the pre-T058 behaviour for a never-touched order, still correct now
// that both tags are explicit.
TEST(LeavesCumQty, PlainCancelOfAnUntouchedRestingOrderReportsFullLeavesAndZeroCum)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 5)}, 1);

  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 2);

  const auto* canceled = cap.first<OrderCanceled>();
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(5));
  EXPECT_EQ(canceled->cumQty, qty(0));
}

// A pending stop, canceled before it ever triggers, never partially fills:
// its full submitted quantity is its LeavesQty and CumQty is 0.
TEST(LeavesCumQty, CancelOfAPendingStopReportsItsFullQuantityAsLeaves)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder stop = order(100, Side::SELL, 100.0, 4);
  stop.type = OrderType::STOP_MARKET;
  stop.triggerPrice = px(90.0);
  eng.submit(InboundCommand{stop}, 1);

  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 2);

  const auto* canceled = cap.first<OrderCanceled>();
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(4));
  EXPECT_EQ(canceled->cumQty, qty(0));
}

// ---- Coverage gap closed: MatchingBook/LadderBook consumeById -------------
// fillFront (MatchingBook)/fillBest (LadderBook) -- the plain FIFO fill path
// -- were already exercised above (the IOC-residual and plain-cancel tests).
// consumeById is a DIFFERENT mutation point on both books, reached only by
// an auction uncross (engine/session.inl) or pro-rata matching (matcher.h's
// crossProRata), neither of which the tests above ever drive. A mutation
// that deletes `order.cumQty += by;` in *_book.h's consumeById passed the
// full suite and golden replay silently until these were added.

// Auction uncross (session.inl:427-428): the SAME four orders and clearing
// price as VenueAuction's "residual" case (test_venue_auction.cpp) -- BUY
// 101x5, SELL 99x5, BUY 100x3, SELL 100x2, uncrossed at 100 for 7 units.
// Order 3 (BUY 100x3) is the one left with a residual: it fills 2 of its 3
// via MatchingBook::consumeById, 1 left resting. Canceling it afterward is
// the only way to observe consumeById's running total on the wire.
TEST(LeavesCumQty, AuctionUncrossPartialFillThenCancelReportsRunningCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.beginPreOpen();
  eng.submit(InboundCommand{order(1, Side::BUY, 101.0, 5)}, 0);
  eng.submit(InboundCommand{order(2, Side::SELL, 99.0, 5)}, 1);
  eng.submit(InboundCommand{order(3, Side::BUY, 100.0, 3)}, 2);
  eng.submit(InboundCommand{order(4, Side::SELL, 100.0, 2)}, 3);
  eng.openContinuous();  // uncross: 7 units at 100, order 3 left with 1

  CancelOrder c;
  c.id = 3;
  c.symbol = SYM;
  c.accountId = 1;  // order() defaults every order here to account 1
  eng.submit(InboundCommand{c}, 4);

  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 3; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(1)) << "3 requested - 2 filled at uncross = 1 left resting";
  EXPECT_EQ(canceled->cumQty, qty(2))
      << "2 filled via MatchingBook::consumeById during the uncross, not fillFront";

  const std::string wire = FixCodec::encode(OutboundEvent{*canceled});
  EXPECT_EQ(tag(wire, 151), "1");
  EXPECT_EQ(tag(wire, 14), "2");
}

// Pro-rata continuous matching (matcher.h:863, crossProRata): a single
// resting maker is consumed by id in the same "allocate per level" loop a
// competing multi-maker level would use -- with one maker, its whole
// allocation is simply its request, but the consumption still goes through
// consumeById exactly as it does with several makers at the level.
TEST(LeavesCumQty, ProRataPartialFillThenCancelReportsRunningCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink(), MatchingBook{}, MatchPolicy::ProRata);

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 10)}, 1);  // resting maker, qty 10
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 4)}, 2);    // pro-rata taker, qty 4

  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 3);

  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 100; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(6)) << "10 - 4 filled pro-rata = 6 left resting";
  EXPECT_EQ(canceled->cumQty, qty(4)) << "4 filled via MatchingBook::consumeById (crossProRata)";

  const std::string wire = FixCodec::encode(OutboundEvent{*canceled});
  EXPECT_EQ(tag(wire, 151), "6");
  EXPECT_EQ(tag(wire, 14), "4");
}

// ---- Same two book-mutation points, on flox::LadderBook --------------------
// The golden-replay corpus instantiates MatchingEngine<MatchingBook> only
// (test_venue_golden_replay.cpp), so it gives LadderBook's fillBest/
// consumeById no coverage at all, mutated or not. These are direct unit
// tests against LadderBook instead, mirroring the MatchingBook cases above.

TEST(LeavesCumQty, LadderBookFifoPartialFillThenCancelReportsRunningCumQty)
{
  Capture cap;
  MatchingEngine<LadderBook> eng(cfg(), cap.sink(), LadderBook{lc()});

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 5)}, 1);  // resting maker, qty 5
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 2)}, 2);   // FIFO taker fills 2 (LadderBook::fillBest)

  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 3);

  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 100; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(3));
  EXPECT_EQ(canceled->cumQty, qty(2)) << "2 filled via LadderBook::fillBest";
}

TEST(LeavesCumQty, LadderBookProRataPartialFillThenCancelReportsRunningCumQty)
{
  Capture cap;
  MatchingEngine<LadderBook> eng(cfg(), cap.sink(), LadderBook{lc()}, MatchPolicy::ProRata);

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 10)}, 1);  // resting maker, qty 10
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 4)}, 2);    // pro-rata taker (LadderBook::consumeById)

  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 3);

  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 100; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(6));
  EXPECT_EQ(canceled->cumQty, qty(4)) << "4 filled via LadderBook::consumeById (crossProRata)";
}

// ---- T059: FIX 14 (CumQty) on OrderAccepted / OrderExecuted / OrderModified
// / FillHeld / FillRejected -- the five reports T058 left without it. See
// the note's audit table (T058) for what was already covered.
//
// A lastLook-enabled config for the FillHeld/FillRejected tests below.
venue::SymbolConfig cfgLastLook()
{
  venue::SymbolConfig c = cfg();
  c.lastLookWindowNs = DurationNs{1'000'000'000};
  return c;
}

// ---- FixCodec / SbeOrderEntryCodec: pure functions of the event -----------

TEST(LeavesCumQty, FixCodecWritesTag14OnOrderAccepted)
{
  OrderAccepted a;
  a.id = 10;
  a.symbol = SYM;
  a.side = Side::BUY;
  a.price = px(100.0);
  a.leavesQty = qty(3);
  a.restingOnBook = true;
  a.account = 1;
  a.cumQty = qty(2);

  const std::string wire = FixCodec::encode(OutboundEvent{a});
  EXPECT_EQ(tag(wire, 14), "2");
}

TEST(LeavesCumQty, SbeCodecWritesCumQtyOnAcceptedAfterClOrdId)
{
  OrderAccepted a;
  a.id = 10;
  a.symbol = SYM;
  a.side = Side::BUY;
  a.price = px(100.0);
  a.leavesQty = qty(3);
  a.restingOnBook = true;
  a.account = 1;
  a.clientOrderId = 5;
  a.cumQty = qty(2);

  std::vector<uint8_t> f;
  SbeOrderEntryCodec::encode(OutboundEvent{a}, f, /*seq=*/1);
  ASSERT_FALSE(f.empty());
  EXPECT_EQ(sbe::readHeader(f.data()).blockLength, SbeOrderEntryCodec::kBlockAccepted);
  // orderId(8) symbol(4) side(1) price(8) leavesQty(8) restingOnBook(1) seq(8) clOrdId(8) cumQty(8)
  EXPECT_EQ(rootU64(f, 38), 5u) << "clOrdId keeps its version-4 offset";
  EXPECT_EQ(static_cast<int64_t>(rootU64(f, 46)), qty(2).raw()) << "cumQty, appended after clOrdId";
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(f.data(), f.size()), 1u);
}

TEST(LeavesCumQty, FixCodecWritesTag14OnOrderExecuted)
{
  OrderExecuted x;
  x.id = 10;
  x.symbol = SYM;
  x.lastQty = qty(2);
  x.leavesQty = qty(3);
  x.aggressor = true;
  x.complete = false;
  x.lastPx = px(100.0);
  x.account = 1;
  x.cumQty = qty(5);

  const std::string wire = FixCodec::encode(OutboundEvent{x});
  EXPECT_EQ(tag(wire, 14), "5");
}

TEST(LeavesCumQty, SbeCodecWritesCumQtyOnExecutedAfterClOrdId)
{
  OrderExecuted x;
  x.id = 10;
  x.symbol = SYM;
  x.lastQty = qty(2);
  x.leavesQty = qty(3);
  x.aggressor = true;
  x.complete = false;
  x.lastPx = px(100.0);
  x.account = 1;
  x.clientOrderId = 6;
  x.cumQty = qty(5);

  std::vector<uint8_t> f;
  SbeOrderEntryCodec::encode(OutboundEvent{x}, f, /*seq=*/2);
  ASSERT_FALSE(f.empty());
  EXPECT_EQ(sbe::readHeader(f.data()).blockLength, SbeOrderEntryCodec::kBlockExecuted);
  // orderId(8) symbol(4) lastQty(8) lastPx(8) leavesQty(8) aggressor(1) complete(1) seq(8) clOrdId(8) cumQty(8)
  EXPECT_EQ(rootU64(f, 46), 6u) << "clOrdId keeps its version-4 offset";
  EXPECT_EQ(static_cast<int64_t>(rootU64(f, 54)), qty(5).raw()) << "cumQty, appended after clOrdId";
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(f.data(), f.size()), 2u);
}

TEST(LeavesCumQty, FixCodecWritesTag14OnOrderModified)
{
  OrderModified m;
  m.id = 10;
  m.symbol = SYM;
  m.price = px(99.0);
  m.leavesQty = qty(6);
  m.account = 1;
  m.cumQty = qty(4);

  const std::string wire = FixCodec::encode(OutboundEvent{m});
  EXPECT_EQ(tag(wire, 14), "4");
}

TEST(LeavesCumQty, SbeCodecWritesCumQtyOnReplacedAfterClOrdId)
{
  OrderModified m;
  m.id = 10;
  m.symbol = SYM;
  m.price = px(99.0);
  m.leavesQty = qty(6);
  m.account = 1;
  m.clientOrderId = 7;
  m.cumQty = qty(4);

  std::vector<uint8_t> f;
  SbeOrderEntryCodec::encode(OutboundEvent{m}, f, /*seq=*/3);
  ASSERT_FALSE(f.empty());
  EXPECT_EQ(sbe::readHeader(f.data()).blockLength, SbeOrderEntryCodec::kBlockReplaced);
  // orderId(8) symbol(4) price(8) leavesQty(8) priorityKept(1) seq(8) clOrdId(8) cumQty(8)
  EXPECT_EQ(rootU64(f, 37), 7u) << "clOrdId keeps its version-4 offset";
  EXPECT_EQ(static_cast<int64_t>(rootU64(f, 45)), qty(4).raw()) << "cumQty, appended after clOrdId";
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(f.data(), f.size()), 3u);
}

TEST(LeavesCumQty, FixCodecWritesTag14OnFillHeldAndFillRejected)
{
  FillHeld fh;
  fh.heldId = 5;
  fh.symbol = SYM;
  fh.makerId = 1;
  fh.takerId = 2;
  fh.price = px(100.0);
  fh.qty = qty(3);
  fh.cumQty = qty(2);
  EXPECT_EQ(tag(FixCodec::encode(OutboundEvent{fh}), 14), "2");

  FillRejected fr;
  fr.heldId = 5;
  fr.symbol = SYM;
  fr.takerId = 2;
  fr.makerId = 1;
  fr.price = px(100.0);
  fr.qty = qty(3);
  fr.cumQty = qty(2);
  EXPECT_EQ(tag(FixCodec::encode(OutboundEvent{fr}), 14), "2");
}

TEST(LeavesCumQty, SbeCodecWritesCumQtyOnFillHeldAndFillRejectedAfterClOrdId)
{
  FillHeld fh;
  fh.heldId = 5;
  fh.symbol = SYM;
  fh.makerId = 1;
  fh.takerId = 2;
  fh.price = px(100.0);
  fh.qty = qty(3);
  fh.clientOrderId = 8;
  fh.cumQty = qty(2);

  std::vector<uint8_t> f;
  SbeOrderEntryCodec::encode(OutboundEvent{fh}, f, /*seq=*/4);
  ASSERT_FALSE(f.empty());
  EXPECT_EQ(sbe::readHeader(f.data()).blockLength, SbeOrderEntryCodec::kBlockFillHeld);
  // heldId(8) symbol(4) makerId(8) takerId(8) price(8) qty(8) makerDisplayAfter(8) seq(8) takerSide(1) clOrdId(8) cumQty(8)
  EXPECT_EQ(rootU64(f, 61), 8u) << "clOrdId keeps its version-7 offset";
  EXPECT_EQ(static_cast<int64_t>(rootU64(f, 69)), qty(2).raw()) << "cumQty, appended after clOrdId";
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(f.data(), f.size()), 4u);

  FillRejected fr;
  fr.heldId = 5;
  fr.symbol = SYM;
  fr.takerId = 2;
  fr.makerId = 1;
  fr.price = px(100.0);
  fr.qty = qty(3);
  fr.clientOrderId = 9;
  fr.cumQty = qty(2);

  std::vector<uint8_t> fr2;
  SbeOrderEntryCodec::encode(OutboundEvent{fr}, fr2, /*seq=*/5);
  ASSERT_FALSE(fr2.empty());
  EXPECT_EQ(sbe::readHeader(fr2.data()).blockLength, SbeOrderEntryCodec::kBlockFillRejected);
  // heldId(8) symbol(4) takerId(8) makerId(8) price(8) qty(8) seq(8) clOrdId(8) cumQty(8)
  EXPECT_EQ(rootU64(fr2, 52), 9u) << "clOrdId keeps its version-7 offset";
  EXPECT_EQ(static_cast<int64_t>(rootU64(fr2, 60)), qty(2).raw()) << "cumQty, appended after clOrdId";
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(fr2.data(), fr2.size()), 5u);
}

// ---- engine integration: OrderAccepted ------------------------------------

// A crossing new order that partially fills BEFORE its residual rests
// (validate.inl's residualRests branch): the accept must carry what it
// already filled of itself, not 0.
TEST(LeavesCumQty, OrderAcceptedAfterAPartialFillOnEntryCarriesCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 3)}, 1);  // resting maker, qty 3
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 5)}, 2);   // crosses 3, rests 2

  const auto* accepted = cap.firstWhere<OrderAccepted>(
      +[](const OrderAccepted& x)
      { return x.id == 200; });
  ASSERT_NE(accepted, nullptr);
  EXPECT_EQ(accepted->leavesQty, qty(2));
  EXPECT_EQ(accepted->cumQty, qty(3)) << "3 filled of itself before this residual rests";
  EXPECT_TRUE(accepted->restingOnBook);

  const std::string wire = FixCodec::encode(OutboundEvent{*accepted});
  EXPECT_EQ(tag(wire, 151), "2");
  EXPECT_EQ(tag(wire, 14), "3");

  // The RestingOrder this order became must also carry the 3 forward, so a
  // later cancel reports the real life-to-date total -- not just the accept.
  CancelOrder c;
  c.id = 200;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 3);
  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 200; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->cumQty, qty(3))
      << "RestingOrder::cumQty must be seeded from the entry fill, not left at 0";
}

// T059 acceptance criteria: a triggered stop that partially fills before its
// residual rests (orders.inl's processTriggers residualRests branch) reports
// FIX 14 > 0 on the accept.
TEST(LeavesCumQty, OrderAcceptedAfterATriggeredStopPartiallyFillsCarriesCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 2)}, 1);  // what the stop will partially fill

  // Set the last-trade reference to 99 first (below the stop's trigger).
  eng.submit(InboundCommand{order(90, Side::SELL, 99.0, 1)}, 2);
  eng.submit(InboundCommand{order(91, Side::BUY, 99.0, 1, /*account=*/2, TimeInForce::IOC)}, 3);

  // A STOP_LIMIT: pending until the reference reaches 100, then aggresses as
  // a LIMIT at 101 -- crosses order 100 (qty 2) and rests the rest.
  NewOrder stop = order(300, Side::BUY, 101.0, 5, /*account=*/3);
  stop.type = OrderType::STOP_LIMIT;
  stop.triggerPrice = px(100.0);
  eng.submit(InboundCommand{stop}, 4);
  ASSERT_TRUE(cap.firstWhere<OrderAccepted>(
                  +[](const OrderAccepted& x)
                  { return x.id == 300 && !x.restingOnBook; }) != nullptr)
      << "parked in the stop book, not yet triggered";

  // Trigger it: a trade prints at 100, moving the last-trade reference to
  // (>=) the stop's trigger.
  eng.submit(InboundCommand{order(95, Side::SELL, 100.0, 1)}, 5);
  eng.submit(InboundCommand{order(96, Side::BUY, 100.0, 1, /*account=*/2, TimeInForce::IOC)}, 6);

  const auto* accepted = cap.firstWhere<OrderAccepted>(
      +[](const OrderAccepted& x)
      { return x.id == 300 && x.restingOnBook; });
  ASSERT_NE(accepted, nullptr) << "the triggered stop's residual accept";
  EXPECT_EQ(accepted->leavesQty, qty(3)) << "5 - 2 filled against order 100";
  EXPECT_GT(accepted->cumQty.raw(), 0) << "T059 acceptance criteria: 14 > 0";
  EXPECT_EQ(accepted->cumQty, qty(2));

  const std::string wire = FixCodec::encode(OutboundEvent{*accepted});
  EXPECT_EQ(tag(wire, 14), "2");
}

// ---- engine integration: OrderExecuted ------------------------------------

// A maker's SECOND partial fill reports the accumulated total, not just this
// fill's own size.
TEST(LeavesCumQty, OrderExecutedOnTheSecondPartialFillCarriesTheAccumulatedCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 10)}, 1);  // resting maker, qty 10
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 3)}, 2);    // fills 3
  eng.submit(InboundCommand{order(201, Side::BUY, 100.0, 2)}, 3);    // fills 2 more

  std::vector<const OrderExecuted*> makerFills;
  for (const auto& e : cap.ev)
  {
    if (const auto* x = std::get_if<OrderExecuted>(&e); x != nullptr && x->id == 100)
    {
      makerFills.push_back(x);
    }
  }
  ASSERT_EQ(makerFills.size(), 2u);
  EXPECT_EQ(makerFills[0]->cumQty, qty(3)) << "first fill: running total is just this fill";
  EXPECT_EQ(makerFills[1]->cumQty, qty(5)) << "second fill: 3 + 2 accumulated";

  const std::string wire = FixCodec::encode(OutboundEvent{*makerFills[1]});
  EXPECT_EQ(tag(wire, 14), "5");

  // The taker leg of the second fill: a fresh order, so its own cumQty is
  // just what it filled in its own (single-fill) sweep.
  const auto* taker2 = cap.firstWhere<OrderExecuted>(
      +[](const OrderExecuted& x)
      { return x.id == 201; });
  ASSERT_NE(taker2, nullptr);
  EXPECT_EQ(taker2->cumQty, qty(2));
}

// ---- engine integration: OrderModified ------------------------------------

// A reduce-in-place amend (same price, shrinking) never trades: cumQty from
// before the modify must be carried through unchanged.
TEST(LeavesCumQty, OrderModifiedReduceInPlaceKeepsTheCumQtyFromBeforeTheModify)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 10)}, 1);  // resting maker
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 4)}, 2);    // fills 4, maker leaves 6

  ModifyOrder m;
  m.id = 100;
  m.symbol = SYM;
  m.newPrice = px(100.0);  // same price -> reduce-in-place
  m.newQty = qty(5);       // <= leaves(6), no hidden reserve
  m.accountId = 1;
  eng.submit(InboundCommand{m}, 3);

  const auto* modified = cap.firstWhere<OrderModified>(
      +[](const OrderModified& x)
      { return x.id == 100 && x.priorityKept; });
  ASSERT_NE(modified, nullptr);
  EXPECT_EQ(modified->leavesQty, qty(5));
  EXPECT_EQ(modified->cumQty, qty(4)) << "unaffected by a reduce, which never trades";

  const std::string wire = FixCodec::encode(OutboundEvent{*modified});
  EXPECT_EQ(tag(wire, 14), "4");
}

// A reprice (re-enter at the tail) that immediately crosses new liquidity:
// the report must carry BOTH the order's pre-modify cumQty AND what this
// re-entering cross just filled -- not either alone.
TEST(LeavesCumQty, OrderModifiedReenterAddsThisCrosssFillsOnTopOfThePriorCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{order(100, Side::SELL, 100.0, 10)}, 1);  // resting maker
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 4)}, 2);    // fills 4, maker leaves 6
  eng.submit(InboundCommand{order(150, Side::BUY, 99.5, 3)}, 3);     // new resting bid, below 100

  ModifyOrder m;
  m.id = 100;
  m.symbol = SYM;
  m.newPrice = px(99.0);  // repriced below 99.5 -> re-enters, crosses order 150
  m.newQty = qty(6);
  m.accountId = 1;
  eng.submit(InboundCommand{m}, 4);

  const auto* modified = cap.firstWhere<OrderModified>(
      +[](const OrderModified& x)
      { return x.id == 100 && !x.priorityKept; });
  ASSERT_NE(modified, nullptr);
  EXPECT_EQ(modified->leavesQty, qty(3)) << "6 - 3 filled against order 150";
  EXPECT_EQ(modified->cumQty, qty(7)) << "4 from before the modify + 3 from this re-entering cross";

  const std::string wire = FixCodec::encode(OutboundEvent{*modified});
  EXPECT_EQ(tag(wire, 14), "7");

  // The re-rested order's own RestingOrder::cumQty must also carry the 7
  // forward, verified via a subsequent cancel.
  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 5);
  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 100; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->cumQty, qty(7));
}

// ---- engine integration: FillHeld / FillRejected --------------------------

// The taker's cumQty on a hold is what it had already confirmed EARLIER in
// the same sweep -- a real fill against a non-last-look maker before the
// hold opened -- never the held quantity itself (still pending).
TEST(LeavesCumQty, FillHeldCarriesTheTakersConfirmedCumQtyFromEarlierInTheSweep)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfgLastLook(), cap.sink());

  eng.submit(InboundCommand{order(90, Side::SELL, 99.0, 2)}, 1);  // plain maker, fills for real

  NewOrder llMaker = order(100, Side::SELL, 100.0, 5, /*account=*/1);
  llMaker.lastLook = true;
  eng.submit(InboundCommand{llMaker}, 2);

  // Taker sweeps both: 2 real against order 90, then up to 4 held against
  // the last-look maker (its whole remaining size).
  eng.submit(InboundCommand{order(200, Side::BUY, 101.0, 6, /*account=*/2)}, 3);

  const auto* held = cap.first<FillHeld>();
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->qty, qty(4));
  EXPECT_EQ(held->cumQty, qty(2)) << "confirmed against order 90 before this hold opened";

  const std::string heldWire = FixCodec::encode(OutboundEvent{*held});
  EXPECT_EQ(tag(heldWire, 14), "2");

  // Rejecting the hold must report the SAME cumQty on FillRejected, and on
  // the report that rebuilds the taker's residual (fully held out of the
  // book, so this is its first-ever accept).
  eng.submit(InboundCommand{LastLookDecision{held->heldId, SYM, /*accept=*/false, {}, 1}}, 4);

  const auto* rejected = cap.first<FillRejected>();
  ASSERT_NE(rejected, nullptr);
  EXPECT_EQ(rejected->cumQty, qty(2));
  EXPECT_EQ(tag(FixCodec::encode(OutboundEvent{*rejected}), 14), "2");

  const auto* rebuiltAccept = cap.firstWhere<OrderAccepted>(
      +[](const OrderAccepted& x)
      { return x.id == 200; });
  ASSERT_NE(rebuiltAccept, nullptr)
      << "the taker's whole order was 2 (real) + 4 (held) = 6, so it never rested before this";
  EXPECT_EQ(rebuiltAccept->cumQty, qty(2));
}

// Same setup, but the taker is IOC: a rejected hold's residual never rests,
// so it cancels instead -- and that cancel's CumQty must be the same
// hold-time snapshot, not 0 (the pre-T059 behaviour).
TEST(LeavesCumQty, FillRejectedIocResidualCancelCarriesTheTakersCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfgLastLook(), cap.sink());

  eng.submit(InboundCommand{order(90, Side::SELL, 99.0, 2)}, 1);

  NewOrder llMaker = order(100, Side::SELL, 100.0, 5, /*account=*/1);
  llMaker.lastLook = true;
  eng.submit(InboundCommand{llMaker}, 2);

  NewOrder taker = order(200, Side::BUY, 101.0, 6, /*account=*/2);
  taker.tif = TimeInForce::IOC;
  eng.submit(InboundCommand{taker}, 3);

  const auto* held = cap.first<FillHeld>();
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->cumQty, qty(2));

  eng.submit(InboundCommand{LastLookDecision{held->heldId, SYM, /*accept=*/false, {}, 1}}, 4);

  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 200; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->leavesQty, qty(4)) << "the held residual, now killed (IOC never rests)";
  EXPECT_EQ(canceled->cumQty, qty(2)) << "T058 left this at 0; T059 threads the hold-time snapshot";
  EXPECT_EQ(tag(FixCodec::encode(OutboundEvent{*canceled}), 14), "2");
}

// A maker that filled for real, THEN had a slice held and REJECTED, must not
// double-count: the book's own optimistic cumQty bump at hold-creation time
// (fillBest runs before the maker's decision is known) has to be undone on
// reject, or every later report on this order overstates its history.
TEST(LeavesCumQty, ARejectedHoldDoesNotInflateTheMakersCumQty)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfgLastLook(), cap.sink());

  NewOrder llMaker = order(100, Side::SELL, 100.0, 10, /*account=*/1);
  llMaker.lastLook = true;
  eng.submit(InboundCommand{llMaker}, 1);

  // Confirmed fill first, via a hold that gets ACCEPTED: order 100 is
  // lastLook, so every fill against it -- this one included -- goes through
  // a hold; accept is what turns it into a real, confirmed print.
  eng.submit(InboundCommand{order(200, Side::BUY, 100.0, 3, /*account=*/2)}, 2);
  const auto* firstHeldPtr = cap.first<FillHeld>();
  ASSERT_NE(firstHeldPtr, nullptr);
  // Copied by value: cap.ev is a vector, and the submit below appends to it,
  // which may reallocate and dangle firstHeldPtr.
  const FillHeld firstHeld = *firstHeldPtr;
  eng.submit(InboundCommand{LastLookDecision{firstHeld.heldId, SYM, /*accept=*/true, {}, 1}}, 3);
  const auto* firstExec = cap.firstWhere<OrderExecuted>(
      +[](const OrderExecuted& x)
      { return x.id == 100; });
  ASSERT_NE(firstExec, nullptr);
  EXPECT_EQ(firstExec->cumQty, qty(3)) << "confirmed: order 100 has really filled 3 so far";

  // Now a SECOND hold on the same maker, this time rejected. Book-side
  // bookkeeping optimistically bumped cumQty by this held qty (2) the
  // moment the hold opened; the reject must undo exactly that.
  eng.submit(InboundCommand{order(201, Side::BUY, 100.0, 2, /*account=*/2)}, 4);
  const FillHeld* secondHeld = nullptr;
  for (const auto& e : cap.ev)
  {
    if (const auto* x = std::get_if<FillHeld>(&e); x != nullptr && x->heldId != firstHeld.heldId)
    {
      secondHeld = x;
    }
  }
  ASSERT_NE(secondHeld, nullptr);
  eng.submit(InboundCommand{LastLookDecision{secondHeld->heldId, SYM, /*accept=*/false, {}, 1}}, 5);

  const auto* restored = cap.firstWhere<OrderModified>(
      +[](const OrderModified& x)
      { return x.id == 100; });
  ASSERT_NE(restored, nullptr) << "order 100 still rests (leaves 7 after the first real fill)";
  EXPECT_EQ(restored->cumQty, qty(3))
      << "still 3 -- the rejected hold's 2 must not have stuck to the running total";

  // Confirmed independently via a plain cancel of what remains.
  CancelOrder c;
  c.id = 100;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 6);
  const auto* canceled = cap.firstWhere<OrderCanceled>(
      +[](const OrderCanceled& x)
      { return x.id == 100; });
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->cumQty, qty(3));
}
