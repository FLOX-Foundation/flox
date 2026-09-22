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
