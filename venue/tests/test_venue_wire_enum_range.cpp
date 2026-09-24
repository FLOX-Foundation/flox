/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Every enum-typed field of an order arrives as one byte, and a byte has 256
// values where an enum has five. This file pins the three places that answer
// for the difference: the SBE decoder refuses a value it cannot name, the
// admission gate does not shift by a number it has not checked, and the
// auction uncross terminates even when handed a mode nobody validated.

#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sbe_order_entry_codec.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
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
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = id;
  return o;
}

// Byte offsets of the enum fields inside an EnterOrder frame, header
// included. Kept as literals rather than derived from the codec: the point
// is to name the wire layout the schema fixes, so a field that silently
// moves shows up here as a changed constant.
constexpr size_t kSideByte = sbe::kHeaderSize + 12;
constexpr size_t kTypeByte = sbe::kHeaderSize + 13;
constexpr size_t kTifByte = sbe::kHeaderSize + 14;
constexpr size_t kStpByte = sbe::kHeaderSize + 16;
constexpr size_t kPegByte = sbe::kHeaderSize + 75;
constexpr size_t kLadderStpByte = sbe::kHeaderSize + 53;
constexpr size_t kLadderTifByte = sbe::kHeaderSize + 57;

std::vector<uint8_t> enterFrame()
{
  std::vector<uint8_t> w;
  SbeOrderEntryCodec::encode(InboundCommand{limit(42, Side::BUY, 100, 5)}, w);
  return w;
}

std::vector<uint8_t> ladderFrame()
{
  QuoteLadder l;
  l.accountId = 7;
  l.symbol = SYM;
  l.bidIdBase = 100;
  l.askIdBase = 200;
  l.levels = 1;
  l.level[0].bidPrice = px(99);
  l.level[0].bidQty = qty(1);
  l.level[0].askPrice = px(101);
  l.level[0].askQty = qty(1);
  std::vector<uint8_t> w;
  SbeOrderEntryCodec::encode(InboundCommand{l}, w);
  return w;
}

// A frame with one byte overwritten -- the wire a client that does not share
// this build's enum tables would put on the socket.
std::vector<uint8_t> withByte(std::vector<uint8_t> w, size_t at, uint8_t v)
{
  w[at] = v;
  return w;
}

struct Cap
{
  std::vector<OutboundEvent> ev;
  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }
  const OrderRejected* lastReject() const
  {
    const OrderRejected* r = nullptr;
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderRejected>(&e))
      {
        r = x;
      }
    }
    return r;
  }
  int canceled() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<OrderCanceled>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  int trades() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<Trade>(&e))
      {
        ++n;
      }
    }
    return n;
  }
};

}  // namespace

// ---- the decoder ----------------------------------------------------------

TEST(WireEnumRange, AValidFrameStillDecodes)
{
  const auto w = enterFrame();
  const char* err = "unset";
  const auto d = SbeOrderEntryCodec::decode(w.data(), w.size(), err);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(err, nullptr);
  const auto* o = std::get_if<NewOrder>(&*d);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->side, Side::BUY);
  EXPECT_EQ(o->type, OrderType::LIMIT);
  EXPECT_EQ(o->tif, TimeInForce::GTC);
  EXPECT_EQ(o->stp, STPMode::None);
  EXPECT_EQ(o->peg, PegRef::None);
}

// Every value each enum names has to survive the round trip, or the range
// check has narrowed the protocol instead of guarding it.
TEST(WireEnumRange, EveryNamedValueRoundTrips)
{
  const OrderType types[] = {OrderType::LIMIT,
                             OrderType::MARKET,
                             OrderType::STOP_MARKET,
                             OrderType::STOP_LIMIT,
                             OrderType::TAKE_PROFIT_MARKET,
                             OrderType::TAKE_PROFIT_LIMIT,
                             OrderType::TRAILING_STOP,
                             OrderType::ICEBERG};
  const TimeInForce tifs[] = {TimeInForce::GTC, TimeInForce::IOC, TimeInForce::FOK,
                              TimeInForce::GTD, TimeInForce::POST_ONLY};
  const STPMode stps[] = {STPMode::None, STPMode::CancelNewest, STPMode::CancelOldest,
                          STPMode::CancelBoth, STPMode::Decrement};
  const PegRef pegs[] = {PegRef::None, PegRef::Bid, PegRef::Ask, PegRef::Mid};

  for (Side s : {Side::BUY, Side::SELL})
  {
    for (OrderType t : types)
    {
      for (TimeInForce f : tifs)
      {
        for (STPMode m : stps)
        {
          for (PegRef p : pegs)
          {
            NewOrder o = limit(1, s, 100, 5);
            o.type = t;
            o.tif = f;
            o.stp = m;
            o.peg = p;
            std::vector<uint8_t> w;
            SbeOrderEntryCodec::encode(InboundCommand{o}, w);
            const char* err = "unset";
            const auto d = SbeOrderEntryCodec::decode(w.data(), w.size(), err);
            ASSERT_TRUE(d.has_value())
                << "refused a named combination: type " << static_cast<int>(t) << " tif "
                << static_cast<int>(f) << " stp " << static_cast<int>(m) << " peg "
                << static_cast<int>(p) << " -- " << (err != nullptr ? err : "no reason");
            const auto* back = std::get_if<NewOrder>(&*d);
            ASSERT_NE(back, nullptr);
            EXPECT_EQ(back->side, s);
            EXPECT_EQ(back->type, t);
            EXPECT_EQ(back->tif, f);
            EXPECT_EQ(back->stp, m);
            EXPECT_EQ(back->peg, p);
          }
        }
      }
    }
  }
}

TEST(WireEnumRange, AnOutOfRangeSideIsRefusedWithAReason)
{
  for (int v = 2; v < 256; ++v)
  {
    const auto w = withByte(enterFrame(), kSideByte, static_cast<uint8_t>(v));
    const char* err = nullptr;
    EXPECT_FALSE(SbeOrderEntryCodec::decode(w.data(), w.size(), err).has_value()) << "side " << v;
    ASSERT_NE(err, nullptr) << "side " << v;
    EXPECT_NE(std::string(err).find("side"), std::string::npos) << err;
  }
}

TEST(WireEnumRange, AnOutOfRangeOrderTypeIsRefusedWithAReason)
{
  for (int v = 8; v < 256; ++v)
  {
    const auto w = withByte(enterFrame(), kTypeByte, static_cast<uint8_t>(v));
    const char* err = nullptr;
    EXPECT_FALSE(SbeOrderEntryCodec::decode(w.data(), w.size(), err).has_value()) << "type " << v;
    ASSERT_NE(err, nullptr) << "type " << v;
    EXPECT_NE(std::string(err).find("type"), std::string::npos) << err;
  }
}

TEST(WireEnumRange, AnOutOfRangeTimeInForceIsRefusedWithAReason)
{
  for (int v = 5; v < 256; ++v)
  {
    const auto w = withByte(enterFrame(), kTifByte, static_cast<uint8_t>(v));
    const char* err = nullptr;
    EXPECT_FALSE(SbeOrderEntryCodec::decode(w.data(), w.size(), err).has_value()) << "tif " << v;
    ASSERT_NE(err, nullptr) << "tif " << v;
    EXPECT_NE(std::string(err).find("tif"), std::string::npos) << err;
  }
}

TEST(WireEnumRange, AnOutOfRangeStpModeIsRefusedWithAReason)
{
  for (int v = 5; v < 256; ++v)
  {
    const auto w = withByte(enterFrame(), kStpByte, static_cast<uint8_t>(v));
    const char* err = nullptr;
    EXPECT_FALSE(SbeOrderEntryCodec::decode(w.data(), w.size(), err).has_value()) << "stp " << v;
    ASSERT_NE(err, nullptr) << "stp " << v;
    EXPECT_NE(std::string(err).find("stp"), std::string::npos) << err;
  }
}

TEST(WireEnumRange, AnOutOfRangePegRefIsRefusedWithAReason)
{
  for (int v = 4; v < 256; ++v)
  {
    const auto w = withByte(enterFrame(), kPegByte, static_cast<uint8_t>(v));
    const char* err = nullptr;
    EXPECT_FALSE(SbeOrderEntryCodec::decode(w.data(), w.size(), err).has_value()) << "peg " << v;
    ASSERT_NE(err, nullptr) << "peg " << v;
    EXPECT_NE(std::string(err).find("peg"), std::string::npos) << err;
  }
}

// The ladder carries the same two enums for every one of its rungs, so an
// unchecked byte there is eight orders wrong instead of one.
TEST(WireEnumRange, AQuoteLadderEnumIsCheckedToo)
{
  {
    const auto w = ladderFrame();
    const char* err = "unset";
    EXPECT_TRUE(SbeOrderEntryCodec::decode(w.data(), w.size(), err).has_value());
    EXPECT_EQ(err, nullptr);
  }
  for (int v = 5; v < 256; ++v)
  {
    const auto s = withByte(ladderFrame(), kLadderStpByte, static_cast<uint8_t>(v));
    const char* err = nullptr;
    EXPECT_FALSE(SbeOrderEntryCodec::decode(s.data(), s.size(), err).has_value()) << "stp " << v;
    ASSERT_NE(err, nullptr) << "stp " << v;
    EXPECT_NE(std::string(err).find("stp"), std::string::npos) << err;

    const auto t = withByte(ladderFrame(), kLadderTifByte, static_cast<uint8_t>(v));
    err = nullptr;
    EXPECT_FALSE(SbeOrderEntryCodec::decode(t.data(), t.size(), err).has_value()) << "tif " << v;
    ASSERT_NE(err, nullptr) << "tif " << v;
    EXPECT_NE(std::string(err).find("tif"), std::string::npos) << err;
  }
}

// ---- the admission gate ---------------------------------------------------

// A profile's allowedTypes is a bitmap indexed by the order type, so an
// order type of 32 or more used to shift past the width of the word it
// indexes. Whatever that shift produces, the answer the gate owes is the
// same: a type the profile did not list is not permitted.
TEST(WireEnumRange, AnOrderTypePastTheProfileBitmapIsNotPermitted)
{
  for (int v : {8, 31, 32, 33, 64, 200, 255})
  {
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink(), MatchingBook{});
    AdmissionProfile p;
    p.allowedTypes = 1u << static_cast<uint32_t>(OrderType::LIMIT);
    eng.setAdmissionProfile(9, p);
    NewOrder o = limit(1, Side::BUY, 100, 5);
    o.accountId = 9;
    o.type = static_cast<OrderType>(v);
    eng.submit(InboundCommand{o}, 1);
    const OrderRejected* r = cap.lastReject();
    ASSERT_NE(r, nullptr) << "order type " << v << " was admitted by a LIMIT-only profile";
    EXPECT_EQ(r->reason, RejectReason::OrderTypeNotPermitted) << "order type " << v;
  }
}

TEST(WireEnumRange, ATimeInForcePastTheProfileBitmapIsNotPermitted)
{
  for (int v : {5, 31, 32, 33, 64, 200, 255})
  {
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink(), MatchingBook{});
    AdmissionProfile p;
    p.allowedTif = 1u << static_cast<uint32_t>(TimeInForce::GTC);
    eng.setAdmissionProfile(9, p);
    NewOrder o = limit(1, Side::BUY, 100, 5);
    o.accountId = 9;
    o.tif = static_cast<TimeInForce>(v);
    eng.submit(InboundCommand{o}, 1);
    const OrderRejected* r = cap.lastReject();
    ASSERT_NE(r, nullptr) << "time in force " << v << " was admitted by a GTC-only profile";
    EXPECT_EQ(r->reason, RejectReason::TimeInForceNotPermitted) << "time in force " << v;
  }
}

// validate()'s price, tick and band checks are keyed on OrderType::LIMIT, so
// a type nobody recognised skipped all of them and rested at whatever price
// the frame carried. It is refused instead -- with no admission profile in
// sight, because this is the venue's answer, not a profile's.
TEST(WireEnumRange, AnUnknownOrderTypeDoesNotRestAtAnUncheckedPrice)
{
  for (int v : {8, 32, 200, 255})
  {
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink(), MatchingBook{});
    NewOrder o = limit(1, Side::BUY, 100, 5);
    o.type = static_cast<OrderType>(v);
    o.price = Price::fromRaw(-1);  // below zero, off tick and outside the band
    eng.submit(InboundCommand{o}, 1);
    EXPECT_EQ(eng.book().find(1), nullptr) << "order type " << v << " rested at price -1";
    const OrderRejected* r = cap.lastReject();
    ASSERT_NE(r, nullptr) << "order type " << v;
    EXPECT_EQ(r->reason, RejectReason::UnknownOrderType) << "order type " << v;
  }
}

// ---- the auction uncross --------------------------------------------------

// An STP mode outside the enum makes auctionVerdict engage without naming an
// action: no decrement, no cancel. The uncross used to re-peek the same pair
// and go round again, forever, with the consumer thread inside it -- one
// crafted frame and the shard stops answering. The loop must make progress
// on every pass, whatever the verdict says.
TEST(WireEnumRange, TheUncrossTerminatesOnAVerdictThatNamesNoAction)
{
  auto body = []()
  {
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink(), MatchingBook{});
    eng.beginPreOpen();
    NewOrder b = limit(11, Side::BUY, 100, 5);
    b.accountId = 42;
    b.stp = static_cast<STPMode>(9);
    NewOrder a = limit(12, Side::SELL, 100, 5);
    a.accountId = 42;
    a.stp = static_cast<STPMode>(9);
    eng.submit(InboundCommand{b}, 0);
    eng.submit(InboundCommand{a}, 1);
    eng.openContinuous();
    // The pair must not print, and neither leg may be left crossing the
    // other: a verdict that cannot say which side to keep keeps neither.
    EXPECT_EQ(cap.trades(), 0);
    EXPECT_EQ(eng.book().find(11), nullptr);
    EXPECT_EQ(eng.book().find(12), nullptr);
    EXPECT_EQ(cap.canceled(), 2);
  };

  // Run it off the test thread: before the guard this never returns, and a
  // test that hangs tells a reader nothing about which scenario did it.
  std::promise<void> done;
  std::future<void> f = done.get_future();
  std::thread([&body, &done]
              {
                body();
                done.set_value(); })
      .detach();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "the auction uncross did not terminate: a verdict that engages without naming an "
         "action sends the loop round the same pair forever";
  f.get();
}
