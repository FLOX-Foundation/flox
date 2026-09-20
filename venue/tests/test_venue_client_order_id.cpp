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

  const size_t clOrdAt = buf.size() - 8;
  uint64_t got = 0;
  std::memcpy(&got, buf.data() + clOrdAt, sizeof got);
  EXPECT_EQ(got, kClientId) << "and the submitter's name is the trailing field";
}
