/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The identifier the submitter gave an order, carried from the request to
 * every report about that order.
 *
 * Why this has its own file: a submitter reconciles reports against the name
 * it chose (ClOrdID, tag 11), not against the one the venue assigned
 * (OrderID, tag 37). The FIX path here happens to set both from the same
 * incoming field, which is why a test written over that path alone cannot
 * tell a correct report from one that copies 37 into 11 -- it passed for
 * exactly that reason before this existed. Every check below therefore uses
 * an order whose two identifiers DIFFER, and asserts which one lands where.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sbe_order_entry_codec.h"

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
constexpr OrderId kVenueId = 5000;       // what the venue calls the order
constexpr uint64_t kClientId = 77;       // what the submitter calls it
constexpr uint64_t kOtherClientId = 91;  // the counterparty's own name for its order

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

// Same as cfg(), plus last look armed -- needed for the FillHeld/FillRejected
// (T050) cases below, which do not exist without a hold.
SymbolConfig cfgWithLastLook()
{
  SymbolConfig c = cfg();
  c.lastLookWindowNs = DurationNs{1000};
  c.lastLookAcceptOnTimeout = false;
  return c;
}

NewOrder order(OrderId id, uint64_t clOrd, Side side, double price, double quantity,
               uint64_t account = 1)
{
  NewOrder o;
  o.id = id;
  o.clientOrderId = clOrd;
  o.symbol = SYM;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = px(price);
  o.quantity = qty(quantity);
  o.accountId = account;
  return o;
}

// One FIX tag out of an encoded message, or "" when the tag is absent. Absent
// and zero are different answers and the tests below rely on the difference.
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

}  // namespace

TEST(ClientOrderId, AcceptReportsTheSubmittersNameAndTheVenuesSeparately)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{order(kVenueId, kClientId, Side::SELL, 100.0, 5)}, 1);

  const auto* a = cap.first<OrderAccepted>();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->id, kVenueId);
  EXPECT_EQ(a->clientOrderId, kClientId);

  const std::string wire = FixCodec::encode(OutboundEvent{*a});
  EXPECT_EQ(tag(wire, 37), std::to_string(kVenueId)) << "37 is the venue's identifier";
  EXPECT_EQ(tag(wire, 11), std::to_string(kClientId)) << "11 is the submitter's";
  EXPECT_NE(tag(wire, 11), tag(wire, 37)) << "and they are not the same field";
}

TEST(ClientOrderId, AnOrderThatGaveNoNameGetsNoTagRatherThanTheVenuesId)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{order(kVenueId, 0, Side::SELL, 100.0, 5)}, 1);

  const auto* a = cap.first<OrderAccepted>();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->clientOrderId, 0u);

  const std::string wire = FixCodec::encode(OutboundEvent{*a});
  EXPECT_EQ(tag(wire, 37), std::to_string(kVenueId));
  EXPECT_TRUE(tag(wire, 11).empty())
      << "no ClOrdID was given, so none is claimed -- 11 must not echo 37";
}

TEST(ClientOrderId, EveryReportInAnOrdersLifeCarriesIt)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{order(kVenueId, kClientId, Side::SELL, 100.0, 5)}, 1);

  ModifyOrder m;
  m.id = kVenueId;
  m.symbol = SYM;
  m.newPrice = px(101.0);
  m.newQty = qty(5);
  m.accountId = 1;
  eng.submit(InboundCommand{m}, 2);

  // A taker with its OWN name takes part of the resting order: both legs get a
  // report and each must be named the way its own submitter named it.
  eng.submit(InboundCommand{order(6000, kOtherClientId, Side::BUY, 101.0, 2)}, 3);

  CancelOrder c;
  c.id = kVenueId;
  c.symbol = SYM;
  c.accountId = 1;
  eng.submit(InboundCommand{c}, 4);

  bool sawModified = false;
  bool sawMakerFill = false;
  bool sawTakerFill = false;
  bool sawCanceled = false;
  for (const auto& e : cap.ev)
  {
    if (const auto* x = std::get_if<OrderModified>(&e); x != nullptr && x->id == kVenueId)
    {
      sawModified = true;
      EXPECT_EQ(x->clientOrderId, kClientId) << "an amend is the same order, under the same name";
    }
    if (const auto* x = std::get_if<OrderExecuted>(&e))
    {
      if (x->id == kVenueId)
      {
        sawMakerFill = true;
        EXPECT_EQ(x->clientOrderId, kClientId);
      }
      if (x->id == 6000)
      {
        sawTakerFill = true;
        EXPECT_EQ(x->clientOrderId, kOtherClientId) << "the other side's own name, not the maker's";
      }
    }
    if (const auto* x = std::get_if<OrderCanceled>(&e); x != nullptr && x->id == kVenueId)
    {
      sawCanceled = true;
      EXPECT_EQ(x->clientOrderId, kClientId);
    }
  }
  EXPECT_TRUE(sawModified);
  EXPECT_TRUE(sawMakerFill);
  EXPECT_TRUE(sawTakerFill);
  EXPECT_TRUE(sawCanceled);
}

TEST(ClientOrderId, ARejectCarriesItToo)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  // Off-tick price: refused at admission, while the request is still in hand.
  NewOrder bad = order(kVenueId, kClientId, Side::SELL, 100.005, 5);
  eng.submit(InboundCommand{bad}, 1);

  const auto* j = cap.first<OrderRejected>();
  ASSERT_NE(j, nullptr);
  EXPECT_EQ(j->id, kVenueId);
  EXPECT_EQ(j->clientOrderId, kClientId)
      << "a reject is where this matters most: there may never be another report to match on";

  const std::string wire = FixCodec::encode(OutboundEvent{*j});
  EXPECT_EQ(tag(wire, 11), std::to_string(kClientId));
}

// A Quote is the one InboundCommand that already splits into children by
// design: one submission becomes two resting orders (bid, ask), each with
// its own venue OrderID. Real reason this matters: a submitter that split a
// parent order into venue-level children has exactly one name for the
// parent and two different venue ids for the results, and today (before this
// carries clientOrderId) it is reduced to matching the two reports by symbol
// and timing -- guessing.
TEST(ClientOrderId, AQuoteSplitIntoTwoLegsSharesOneClOrdIdWithDifferentOrderIds)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  constexpr OrderId kBidId = 8001;
  constexpr OrderId kAskId = 8002;
  Quote q;
  q.bidId = kBidId;
  q.askId = kAskId;
  q.symbol = SYM;
  q.bidPrice = px(99.0);
  q.bidQty = qty(1);
  q.askPrice = px(101.0);
  q.askQty = qty(1);
  q.accountId = 1;
  q.clientOrderId = kClientId;
  eng.submit(InboundCommand{q}, 1);

  const OrderAccepted* bid = nullptr;
  const OrderAccepted* ask = nullptr;
  for (const auto& e : cap.ev)
  {
    if (const auto* a = std::get_if<OrderAccepted>(&e))
    {
      if (a->id == kBidId)
      {
        bid = a;
      }
      if (a->id == kAskId)
      {
        ask = a;
      }
    }
  }
  ASSERT_NE(bid, nullptr);
  ASSERT_NE(ask, nullptr);
  EXPECT_NE(bid->id, ask->id) << "two different venue order ids -- the whole point of a split";
  EXPECT_EQ(bid->clientOrderId, kClientId);
  EXPECT_EQ(ask->clientOrderId, kClientId) << "one ClOrdID names both children of the split";

  const std::string bidWire = FixCodec::encode(OutboundEvent{*bid});
  const std::string askWire = FixCodec::encode(OutboundEvent{*ask});
  EXPECT_EQ(tag(bidWire, 11), std::to_string(kClientId));
  EXPECT_EQ(tag(askWire, 11), std::to_string(kClientId));
  EXPECT_EQ(tag(bidWire, 37), std::to_string(kBidId));
  EXPECT_EQ(tag(askWire, 37), std::to_string(kAskId));
  EXPECT_NE(tag(bidWire, 37), tag(askWire, 37)) << "different OrderIDs, same ClOrdID -- normal for a split";
}

// A quote's clientOrderId is checked once, for the pair -- not once per leg
// (that would refuse the ask leg for repeating the bid leg's just-registered
// id). Confirms the once-per-quote check still catches a GENUINE resend: a
// second, unrelated quote reusing the same clOrdId is refused exactly like a
// resent NewOrder would be, and neither of its legs reach the book.
TEST(ClientOrderId, AQuoteResendUnderTheSameClOrdIdIsRefusedNotAccepted)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  Quote first;
  first.bidId = 9001;
  first.askId = 9002;
  first.symbol = SYM;
  first.bidPrice = px(99.0);
  first.bidQty = qty(1);
  first.askPrice = px(101.0);
  first.askQty = qty(1);
  first.accountId = 1;
  first.clientOrderId = kClientId;
  eng.submit(InboundCommand{first}, 1);

  cap.ev.clear();
  Quote resend = first;
  resend.bidId = 9101;  // different venue ids, same clOrdId -> a resend, not a split
  resend.askId = 9102;
  eng.submit(InboundCommand{resend}, 2);

  bool refused = false;
  for (const auto& e : cap.ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      refused = refused || r->reason == RejectReason::DuplicateClientOrderId;
    }
    EXPECT_EQ(std::get_if<OrderAccepted>(&e), nullptr)
        << "the resend's legs must not reach the book";
  }
  EXPECT_TRUE(refused);
}

// The binary path carries the same value, and appending it must not disturb
// the sequence number that was appended before it. Reading seq from the last
// eight bytes of the block was the shortcut that this append broke.
TEST(ClientOrderId, TheBinaryReportCarriesItWithoutDisplacingTheSequence)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{order(kVenueId, kClientId, Side::SELL, 100.0, 5)}, 1);
  const auto* a = cap.first<OrderAccepted>();
  ASSERT_NE(a, nullptr);

  std::vector<uint8_t> buf;
  SbeOrderEntryCodec::encode(OutboundEvent{*a}, buf, /*seq=*/42);
  EXPECT_EQ(SbeOrderEntryCodec::seqOf(buf.data(), buf.size()), 42u)
      << "the sequence number is still where a reader looks for it";

  // T059: cumQty (i64) now trails clOrdId on Accepted, so clOrdId is the
  // second-to-last field, not the last.
  const size_t clOrdAt = buf.size() - 16;
  uint64_t got = 0;
  std::memcpy(&got, buf.data() + clOrdAt, sizeof got);
  EXPECT_EQ(got, kClientId) << "and the submitter's name is the trailing field";
}

// T050: a last-look hold and its reject name the TAKER's order, not the
// maker's and not the venue's own heldId. The maker and taker are given
// DIFFERENT client ids on purpose (same reasoning as the file header): a
// report that echoed the maker's name, or 37/heldId, or nothing at all
// (clientOrderId left 0) would still pass a test that never distinguished
// them.
TEST(ClientOrderId, AHeldFillAndItsRejectCarryTheTakersName)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfgWithLastLook(), cap.sink());

  NewOrder maker = order(kVenueId, kOtherClientId, Side::SELL, 100.0, 5);
  maker.lastLook = true;
  eng.submit(InboundCommand{maker}, 1);

  constexpr OrderId kTakerId = 6001;
  eng.submit(InboundCommand{order(kTakerId, kClientId, Side::BUY, 100.0, 3, /*account=*/2)}, 2);

  const auto* heldPtr = cap.first<FillHeld>();
  ASSERT_NE(heldPtr, nullptr);
  EXPECT_EQ(heldPtr->takerId, kTakerId);
  EXPECT_EQ(heldPtr->clientOrderId, kClientId)
      << "the taker's own name, not the maker's (" << kOtherClientId << ") and not 0";

  const std::string heldWire = FixCodec::encode(OutboundEvent{*heldPtr});
  EXPECT_EQ(tag(heldWire, 11), std::to_string(kClientId));

  std::vector<uint8_t> heldBuf;
  SbeOrderEntryCodec::encode(OutboundEvent{*heldPtr}, heldBuf, /*seq=*/1);
  EXPECT_EQ(heldBuf.size(), sbe::kHeaderSize + SbeOrderEntryCodec::kBlockFillHeld);
  // T059: cumQty (i64) now trails clOrdId on FillHeld.
  uint64_t heldClOrd = 0;
  std::memcpy(&heldClOrd, heldBuf.data() + heldBuf.size() - 16, sizeof heldClOrd);
  EXPECT_EQ(heldClOrd, kClientId) << "second-to-last field of the FillHeld root block";

  // Copied by value before the next submit(): cap.ev is a vector, and the
  // decision below appends to it, which may reallocate and dangle heldPtr.
  const FillHeld held = *heldPtr;

  // Decided by the MAKER's account (1, the default of order() above) -- only
  // the account owning the held quote may answer.
  eng.submit(InboundCommand{LastLookDecision{held.heldId, SYM, /*accept=*/false, {}, 1}}, 3);

  const auto* rejected = cap.first<FillRejected>();
  ASSERT_NE(rejected, nullptr);
  EXPECT_EQ(rejected->takerId, kTakerId);
  EXPECT_EQ(rejected->clientOrderId, kClientId)
      << "the reject is often the only report a client gets for this fill";

  const std::string rejWire = FixCodec::encode(OutboundEvent{*rejected});
  EXPECT_EQ(tag(rejWire, 11), std::to_string(kClientId));

  std::vector<uint8_t> rejBuf;
  SbeOrderEntryCodec::encode(OutboundEvent{*rejected}, rejBuf, /*seq=*/2);
  EXPECT_EQ(rejBuf.size(), sbe::kHeaderSize + SbeOrderEntryCodec::kBlockFillRejected);
  // T059: cumQty (i64) now trails clOrdId on FillRejected.
  uint64_t rejClOrd = 0;
  std::memcpy(&rejClOrd, rejBuf.data() + rejBuf.size() - 16, sizeof rejClOrd);
  EXPECT_EQ(rejClOrd, kClientId) << "second-to-last field of the FillRejected root block";
}

// An order that gave no name gets no tag on a hold either -- same rule as
// every other report (AnOrderThatGaveNoNameGetsNoTagRatherThanTheVenuesId
// above), checked here because a hold has an id of its own (heldId) that a
// sloppy implementation could echo into 11 instead of leaving it absent.
TEST(ClientOrderId, AHeldFillFromAnUnnamedTakerClaimsNoTag)
{
  Capture cap;
  MatchingEngine<MatchingBook> eng(cfgWithLastLook(), cap.sink());

  NewOrder maker = order(kVenueId, kOtherClientId, Side::SELL, 100.0, 5);
  maker.lastLook = true;
  eng.submit(InboundCommand{maker}, 1);
  eng.submit(InboundCommand{order(6002, /*clOrd=*/0, Side::BUY, 100.0, 3, /*account=*/2)}, 2);

  const auto* held = cap.first<FillHeld>();
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->clientOrderId, 0u);
  const std::string wire = FixCodec::encode(OutboundEvent{*held});
  EXPECT_TRUE(tag(wire, 11).empty());
}

// The frame says which version it is, and a reader has to go by ">=", not
// "==" -- a peer one generation ahead of this build (kVersion + 1, never a
// literal) still lays FillHeld/FillRejected out exactly as this version does
// up through clOrdId, so seq and clOrdId must still be found at the same
// offsets. This is the failure mode "==" would reintroduce: a genuinely
// forward-compatible frame refused (seq misread as 0) by a reader that only
// recognised its own exact version number.
TEST(ClientOrderId, AFrameOneSchemaVersionAheadStillLocatesSeq)
{
  FillRejected fr{};
  fr.heldId = 5;
  fr.symbol = SYM;
  fr.takerId = 6001;
  fr.makerId = kVenueId;
  fr.price = px(100.0);
  fr.qty = qty(3);
  fr.clientOrderId = kClientId;

  std::vector<uint8_t> buf;
  SbeOrderEntryCodec::encode(OutboundEvent{fr}, buf, /*seq=*/9);
  ASSERT_GE(buf.size(), sbe::kHeaderSize);
  const uint16_t foreignVersion = SbeOrderEntryCodec::kVersion + 1;
  buf[6] = static_cast<uint8_t>(foreignVersion & 0xFF);
  buf[7] = static_cast<uint8_t>((foreignVersion >> 8) & 0xFF);

  EXPECT_EQ(SbeOrderEntryCodec::seqOf(buf.data(), buf.size()), 9u);

  FillHeld fh{};
  fh.heldId = 5;
  fh.symbol = SYM;
  fh.makerId = kVenueId;
  fh.takerId = 6001;
  fh.price = px(100.0);
  fh.qty = qty(3);
  fh.takerSide = Side::BUY;
  fh.clientOrderId = kClientId;

  std::vector<uint8_t> buf2;
  SbeOrderEntryCodec::encode(OutboundEvent{fh}, buf2, /*seq=*/10);
  ASSERT_GE(buf2.size(), sbe::kHeaderSize);
  buf2[6] = static_cast<uint8_t>(foreignVersion & 0xFF);
  buf2[7] = static_cast<uint8_t>((foreignVersion >> 8) & 0xFF);

  EXPECT_EQ(SbeOrderEntryCodec::seqOf(buf2.data(), buf2.size()), 10u);
}
