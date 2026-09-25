/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * engine::Publications: the outbound stream and the per-account resting-order
 * index, against a fake sink.
 *
 * The assertions are over SERIALIZED BYTES, not over fields. A test that reads
 * `ev.reason` and `ev.untilNs` back one at a time only checks the fields
 * someone thought to name; the field that stops being filled is, by
 * construction, the one nobody named. Serializing the whole event and
 * comparing the buffer has no such blind spot -- every byte of every event is
 * in the comparison whether the author remembered it or not, which is also
 * what the golden replay's stream digest does, one level down.
 *
 * The expectation is written as the events themselves and serialized by the
 * same function, so the test says what the stream MEANS while still comparing
 * what it IS.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 7;
constexpr SymbolId OTHER_SYM = 8;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg(uint32_t maxOpenOrders = 0)
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.maxOpenOrders = maxOpenOrders;
  return c;
}

NewOrder limitOrder(OrderId id, Side side, double price, double quantity, uint64_t acct,
                    uint64_t clOrdId = 0)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = px(price);
  o.quantity = qty(quantity);
  o.accountId = acct;
  o.clientOrderId = clOrdId;
  return o;
}

// ---- the byte form -------------------------------------------------------
//
// A field-by-field encoding, little-endian, fixed width, with the variant
// index in front. Not a wire format and not meant to be one -- it exists so
// two event streams can be compared as buffers, and so a field that stops
// being filled changes the buffer.
//
// Alternatives outside this test's scope contribute their tag and nothing
// else: they are formatted by components this test is not about, and their
// presence and position in the stream is all it has an opinion on.

void put(std::vector<uint8_t>& out, uint64_t v, size_t width)
{
  for (size_t i = 0; i < width; ++i)
  {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFU));
  }
}

void putU8(std::vector<uint8_t>& out, uint8_t v) { put(out, v, 1); }
void putU32(std::vector<uint8_t>& out, uint32_t v) { put(out, v, 4); }
void putU64(std::vector<uint8_t>& out, uint64_t v) { put(out, v, 8); }
void putI64(std::vector<uint8_t>& out, int64_t v) { put(out, static_cast<uint64_t>(v), 8); }

void serializeInto(const OutboundEvent& e, std::vector<uint8_t>& out)
{
  putU32(out, static_cast<uint32_t>(e.index()));
  if (const auto* a = std::get_if<OrderAccepted>(&e))
  {
    putU64(out, a->id);
    putU32(out, a->symbol);
    putU8(out, static_cast<uint8_t>(a->side));
    putI64(out, a->price.raw());
    putI64(out, a->leavesQty.raw());
    putU8(out, a->restingOnBook ? 1U : 0U);
    putI64(out, a->displayQty.raw());
    putU64(out, a->account);
    putU64(out, a->clientOrderId);
  }
  else if (const auto* c = std::get_if<OrderCanceled>(&e))
  {
    putU64(out, c->id);
    putU32(out, c->symbol);
    putU8(out, static_cast<uint8_t>(c->reason));
    putU64(out, c->account);
    putU64(out, c->clientOrderId);
  }
  else if (const auto* s = std::get_if<TradingStatusChanged>(&e))
  {
    putU32(out, s->symbol);
    putU8(out, static_cast<uint8_t>(s->status));
    putU8(out, static_cast<uint8_t>(s->reason));
    putI64(out, s->untilNs);
  }
  else if (const auto* d = std::get_if<DerivativesUpdated>(&e))
  {
    putU32(out, d->symbol);
    putI64(out, d->mark.raw());
    putI64(out, d->fundingRateRaw);
    putI64(out, d->nextFundingNs.raw());
    putI64(out, d->openInterest.raw());
  }
  static_assert(std::variant_size_v<OutboundEvent> == 17,
                "new OutboundEvent alternative: serialize it above if a publication this test "
                "owns produces it, or leave it tag-only on purpose");
}

std::vector<uint8_t> serialize(const std::vector<OutboundEvent>& evs)
{
  std::vector<uint8_t> out;
  for (const OutboundEvent& e : evs)
  {
    serializeInto(e, out);
  }
  return out;
}

std::string hex(const std::vector<uint8_t>& b)
{
  std::string s;
  char buf[4];
  for (uint8_t x : b)
  {
    std::snprintf(buf, sizeof(buf), "%02x", x);
    s += buf;
  }
  return s;
}

// The fake sink: it keeps the stream and nothing else.
struct Fake
{
  std::vector<OutboundEvent> ev;

  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }

  void clear() { ev.clear(); }

  template <class T>
  std::vector<OutboundEvent> only() const
  {
    std::vector<OutboundEvent> out;
    for (const OutboundEvent& e : ev)
    {
      if (std::get_if<T>(&e) != nullptr)
      {
        out.push_back(e);
      }
    }
    return out;
  }
};

#define EXPECT_STREAM(actual, ...)                            \
  do                                                          \
  {                                                           \
    const std::vector<OutboundEvent> _want{__VA_ARGS__};      \
    EXPECT_EQ(hex(serialize(actual)), hex(serialize(_want))); \
  } while (false)

using Engine = MatchingEngine<MatchingBook>;

}  // namespace

// ---- instrument-wide publications ---------------------------------------

// Transitions and only transitions. The second halt of an already halted
// symbol is not news, and a subscriber that had to de-duplicate the feed
// itself would have to know the engine's state machine to do it.
TEST(VenuePublications, StatusStreamCarriesTransitionsOnly)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, int64_t{1000});
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, int64_t{2000});
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Resume}}, int64_t{3000});
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Resume}}, int64_t{4000});

  EXPECT_STREAM(f.only<TradingStatusChanged>(),
                TradingStatusChanged{SYM, TradingStatus::Halted, TradingStatusReason::Administrative, 0},
                TradingStatusChanged{SYM, TradingStatus::Trading, TradingStatusReason::Administrative, 0});
}

// A session boundary and an auction phase are different reasons for a
// different status, and both ride the same publication.
TEST(VenuePublications, StatusStreamNamesTheReasonAndTheSymbol)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::CloseSession}}, int64_t{1000});
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::OpenSession}}, int64_t{2000});
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::BeginPreOpen}}, int64_t{3000});

  EXPECT_STREAM(f.only<TradingStatusChanged>(),
                TradingStatusChanged{SYM, TradingStatus::Closed, TradingStatusReason::Session, 0},
                TradingStatusChanged{SYM, TradingStatus::Trading, TradingStatusReason::Session, 0},
                TradingStatusChanged{SYM, TradingStatus::AuctionPreOpen, TradingStatusReason::Auction, 0});
}

// The derivatives publication is formatted here and timed by clearing: it
// carries the mark clearing was given, the rate it last applied, the boundary
// of the calendar it keeps, and the open interest it tracks.
TEST(VenuePublications, DerivativesPublicationCarriesTheWholeLayer)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  constexpr int64_t kInterval = 1'000'000'000;
  constexpr int64_t kFirstBoundary = 5'000'000'000;
  constexpr int64_t kOneBp = kFundingRateScale / 10'000;

  eng.submit(InboundCommand{SetFundingSchedule{SYM, {}, DurationNs{kInterval}, SeqNanos::fromRaw(kFirstBoundary)}},
             int64_t{1000});
  // No mark yet: an unmarked instrument publishes nothing at all.
  EXPECT_TRUE(f.only<DerivativesUpdated>().empty());

  eng.submit(InboundCommand{SetMark{SYM, {}, px(100.0)}}, int64_t{2000});
  eng.submit(InboundCommand{ApplyFunding{SYM, {}, 0.0001, px(101.0)}}, int64_t{3000});

  EXPECT_STREAM(f.only<DerivativesUpdated>(),
                DerivativesUpdated{SYM, px(100.0), 0, SeqNanos::fromRaw(kFirstBoundary), Quantity{}},
                DerivativesUpdated{SYM, px(101.0), kOneBp,
                                   SeqNanos::fromRaw(kFirstBoundary + kInterval), Quantity{}});
}

// ---- per-account resting-order tracking ---------------------------------

// The index answers "which ids does this account hold", so a mass cancel
// reaches exactly them, in id order, each one naming the identifier its
// submitter chose -- and leaves every other account alone.
TEST(VenuePublications, MassCancelSweepsOneAccountInIdOrder)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  // Submitted out of id order on purpose: the sweep must not inherit the
  // insertion order, nor the hash-bucket order of the index it walks.
  eng.submit(InboundCommand{limitOrder(30, Side::BUY, 10.0, 1.0, 100, 9003)}, int64_t{1000});
  eng.submit(InboundCommand{limitOrder(10, Side::BUY, 11.0, 2.0, 100, 9001)}, int64_t{2000});
  eng.submit(InboundCommand{limitOrder(20, Side::BUY, 12.0, 3.0, 100, 9002)}, int64_t{3000});
  eng.submit(InboundCommand{limitOrder(40, Side::BUY, 13.0, 4.0, 200, 9004)}, int64_t{4000});
  ASSERT_EQ(eng.restingOrderCount(), 4U);

  f.clear();
  eng.submit(InboundCommand{MassCancel{100, SYM}}, int64_t{5000});

  EXPECT_STREAM(f.ev,
                OrderCanceled{10, SYM, CancelReason::UserRequested, 100, 9001},
                OrderCanceled{20, SYM, CancelReason::UserRequested, 100, 9002},
                OrderCanceled{30, SYM, CancelReason::UserRequested, 100, 9003});
  EXPECT_EQ(eng.restingOrderCount(), 1U);
  EXPECT_EQ(eng.book().find(40) != nullptr, true);
}

// An account with nothing resting, and a mass cancel addressed to another
// instrument: both are silence, not a partial sweep.
TEST(VenuePublications, MassCancelIgnoresOtherAccountsAndOtherSymbols)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  eng.submit(InboundCommand{limitOrder(10, Side::BUY, 11.0, 2.0, 100, 9001)}, int64_t{1000});
  f.clear();

  eng.submit(InboundCommand{MassCancel{999, SYM}}, int64_t{2000});
  eng.submit(InboundCommand{MassCancel{100, OTHER_SYM}}, int64_t{3000});

  EXPECT_STREAM(f.ev);
  EXPECT_EQ(eng.restingOrderCount(), 1U);

  // Symbol 0 is the all-instruments address, and it does reach this one.
  eng.submit(InboundCommand{MassCancel{100, 0}}, int64_t{4000});
  EXPECT_STREAM(f.ev, OrderCanceled{10, SYM, CancelReason::UserRequested, 100, 9001});
  EXPECT_EQ(eng.restingOrderCount(), 0U);
}

// The count is the index's, not the book's: an order that leaves by being
// filled has to leave the index too. maxOpenOrders is the observable that
// says so -- a per-account counter that only ever grows locks the account out
// of the venue after maxOpenOrders lifetime orders.
TEST(VenuePublications, PerAccountCountFallsWhenAnOrderIsFilled)
{
  Fake f;
  Engine eng(cfg(/*maxOpenOrders=*/2), f.sink());

  eng.submit(InboundCommand{limitOrder(1, Side::BUY, 10.0, 1.0, 100, 1)}, int64_t{1000});
  eng.submit(InboundCommand{limitOrder(2, Side::BUY, 10.0, 1.0, 100, 2)}, int64_t{2000});

  // At the cap: the third is refused and nothing of it reaches the index.
  f.clear();
  eng.submit(InboundCommand{limitOrder(3, Side::BUY, 10.0, 1.0, 100, 3)}, int64_t{3000});
  EXPECT_EQ(f.only<OrderRejected>().size(), 1U);
  EXPECT_EQ(eng.restingOrderCount(), 2U);

  // Fill one of them outright (the counterparty is a different account, so its
  // own cap is untouched).
  eng.submit(InboundCommand{limitOrder(4, Side::SELL, 10.0, 1.0, 200, 4)}, int64_t{4000});
  EXPECT_EQ(eng.restingOrderCount(), 1U);

  // Room again.
  f.clear();
  eng.submit(InboundCommand{limitOrder(5, Side::BUY, 10.0, 1.0, 100, 5)}, int64_t{5000});
  EXPECT_EQ(f.only<OrderRejected>().size(), 0U);
  EXPECT_EQ(f.only<OrderAccepted>().size(), 1U);
  EXPECT_EQ(eng.restingOrderCount(), 2U);
}

// A cancel and a partial fill move the index the same way a full fill does:
// the id stays while anything of it rests, and goes when nothing does.
TEST(VenuePublications, PartialFillKeepsTheIdAndCancelDropsIt)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  eng.submit(InboundCommand{limitOrder(1, Side::BUY, 10.0, 5.0, 100, 11)}, int64_t{1000});
  eng.submit(InboundCommand{limitOrder(2, Side::SELL, 10.0, 2.0, 200, 12)}, int64_t{2000});
  EXPECT_EQ(eng.restingOrderCount(), 1U);  // the maker still has 3 resting

  const Engine::AccountSnapshot before = eng.snapshotAccount(100);
  ASSERT_EQ(before.openOrders.size(), 1U);
  EXPECT_EQ(before.openOrders[0].id, OrderId{1});

  f.clear();
  eng.submit(InboundCommand{CancelOrder{1, SYM, {}, 100}}, int64_t{3000});
  EXPECT_STREAM(f.ev, OrderCanceled{1, SYM, CancelReason::UserRequested, 100, 11});
  EXPECT_EQ(eng.restingOrderCount(), 0U);
  EXPECT_TRUE(eng.snapshotAccount(100).openOrders.empty());
}

// The emergency sweep walks the same index across every account, and in id
// order for the same reason the per-account one does.
TEST(VenuePublications, HaltAndCancelAllSweepsEveryAccountInIdOrder)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  eng.submit(InboundCommand{limitOrder(30, Side::BUY, 10.0, 1.0, 200, 9003)}, int64_t{1000});
  eng.submit(InboundCommand{limitOrder(10, Side::BUY, 11.0, 1.0, 100, 9001)}, int64_t{2000});
  eng.submit(InboundCommand{limitOrder(20, Side::BUY, 12.0, 1.0, 300, 9002)}, int64_t{3000});

  f.clear();
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::HaltAndCancelAll}}, int64_t{4000});

  // The halt is published before the sweep: a subscriber must know the
  // instrument stopped trading before it is told what that cost it.
  EXPECT_STREAM(f.ev,
                TradingStatusChanged{SYM, TradingStatus::Halted, TradingStatusReason::Administrative, 0},
                OrderCanceled{10, SYM, CancelReason::VenueHalt, 100, 9001},
                OrderCanceled{20, SYM, CancelReason::VenueHalt, 300, 9002},
                OrderCanceled{30, SYM, CancelReason::VenueHalt, 200, 9003});
  EXPECT_EQ(eng.restingOrderCount(), 0U);
}

// ---- the index a snapshot clone is handed --------------------------------

// A checkpoint clones the engine on the consumer thread and serializes the
// clone in the background (MatchingEngine::cloneForSnapshot), and the clone
// receives the whole resting-order index through Publications::copyStateFrom.
//
// The gauge is deliberately NOT read off that index -- it is published to an
// atomic, because the reader is the /metrics thread -- which is exactly why an
// index that arrives without its count arrives silently: nothing in the clone
// disagrees with anything, the book is full and the number is zero. That is
// the number an operator scrapes for as long as the background write lasts.
TEST(VenuePublications, ACloneReportsTheRestingOrdersItWasHandedNotZero)
{
  Fake f;
  Engine eng(cfg(), f.sink());

  constexpr int kResting = 6;
  const auto ownerOf = [](int i)
  { return static_cast<uint64_t>(100 + (i % 3)); };
  for (int i = 0; i < kResting; ++i)
  {
    eng.submit(InboundCommand{limitOrder(static_cast<OrderId>(10 + i), Side::BUY,
                                         10.0 + 0.01 * i, 1.0, ownerOf(i))},
               int64_t{1000} + i);
  }
  ASSERT_EQ(eng.restingOrderCount(), static_cast<uint64_t>(kResting));

  const auto clone = eng.cloneForSnapshot();
  ASSERT_NE(clone.engine, nullptr);

  // What the copy holds, counted off the clone's own book rather than off the
  // gauge that is under test.
  uint64_t inBook = 0;
  clone.engine->book().forEachOrder([&inBook](const auto&)
                                    { ++inBook; });
  ASSERT_EQ(inBook, static_cast<uint64_t>(kResting));

  EXPECT_EQ(clone.engine->restingOrderCount(), inBook)
      << "the clone was handed the resting-order index without the count that reports it";
  EXPECT_EQ(clone.engine->restingOrderCount(), eng.restingOrderCount());

  // And the index it was handed really holds those ids: cancelling them on the
  // clone takes its count down by exactly one each time, and the clone knows
  // who owns each one.
  for (int i = 0; i < kResting; ++i)
  {
    CancelOrder c;
    c.id = static_cast<OrderId>(10 + i);
    c.symbol = SYM;
    c.accountId = ownerOf(i);
    clone.engine->submit(InboundCommand{c}, int64_t{5000} + i);
    EXPECT_EQ(clone.engine->restingOrderCount(), static_cast<uint64_t>(kResting - 1 - i));
  }

  // None of which the live engine felt.
  EXPECT_EQ(eng.restingOrderCount(), static_cast<uint64_t>(kResting));
}
