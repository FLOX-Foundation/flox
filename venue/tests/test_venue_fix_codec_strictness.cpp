/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FixCodec strictness: the three places the inbound codec guesses instead of
 * refusing, and the place two different senders are handed the same order-id
 * block.
 *
 * fix_codec.h:13-17 states the contract these tests hold it to: "Required
 * enum/quantity fields are strict -- a missing or invalid Side (54), a
 * present-but-unknown OrdType (40), or a missing OrderQty (38) rejects the
 * message rather than guessing a default." Side and OrdType honour it.
 * TimeInForce (59) does not -- everything outside 3/4 becomes GTC, so FIX
 * GTD (59=6) rests forever -- and ClOrdID (11) / OrigClOrdID (41) / Account
 * (1) / Symbol (55) do not either: they go through bare strtoull/strtoul, so
 * a String value the FIX spec allows becomes 0, and an overflowing one
 * becomes UINT64_MAX, both without a word to the sender.
 *
 * Each group carries a control that passes on today's codec and must keep
 * passing: the strictness is an addition to the mapping, not a rewrite of it.
 */
#include "flox-venue/fix_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t ACCT = 7;

std::string field(int tag, const std::string& v)
{
  return std::to_string(tag) + "=" + v + std::string(1, FixCodec::SOH);
}

// A refusal has to say WHAT it refused: the session layer answers a bad
// inbound message with Text (58) (fix_session.h sendQuoteStatus, and the
// `text` parameter of FixCodec::encode), and "malformed" tells a market
// maker nothing about which of its tags to fix. Today decode() answers
// std::nullopt and nothing else, so these tests are written against the
// two-argument decode the codec needs:
//
//   needs: static std::optional<InboundCommand>
//          FixCodec::decode(const std::string& msg, std::string* reason);
//
// `*reason` is set on every refusal and names the refused field as
// "<FixFieldName>(<tag>)" -- "TimeInForce(59)", "ClOrdID(11)". Detected
// rather than called directly so the missing overload shows up as a failed
// assertion in the run below, not as a build error.
template <typename C, typename = void>
struct HasDecodeWithReason : std::false_type
{
};
template <typename C>
struct HasDecodeWithReason<C, std::void_t<decltype(C::decode(std::declval<const std::string&>(),
                                                             std::declval<std::string*>()))>>
    : std::true_type
{
};

// A template so the `if constexpr` really discards the branch that does not
// compile yet: in a plain function both arms are instantiated.
template <typename C = FixCodec>
std::optional<InboundCommand> decodeWithReason(const std::string& msg, std::string& reason)
{
  reason.clear();
  if constexpr (HasDecodeWithReason<C>::value)
  {
    return C::decode(msg, &reason);
  }
  else
  {
    return C::decode(msg);
  }
}

// The reason names the field when it carries both the FIX field name and the
// tag number -- either alone is ambiguous in a Text a human reads.
testing::AssertionResult reasonNames(const std::string& reason, const char* name, const char* tag)
{
  if (reason.find(name) != std::string::npos && reason.find(tag) != std::string::npos)
  {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << "reason \"" << reason << "\" does not name " << name << "("
                                     << tag << ")";
}

// NewOrderSingle (35=D) body, no header/checksum -- decode() is lenient about
// an absent CheckSum for exactly this kind of test. Every field a valid order
// needs, with the two this file varies passed in.
std::string newOrder(const std::string& clOrdId, const std::vector<std::pair<int, std::string>>& extra = {},
                     const std::string& account = std::to_string(ACCT),
                     const std::string& symbol = std::to_string(SYM))
{
  std::string b = field(35, "D");
  b += field(11, clOrdId);
  b += field(1, account);
  b += field(55, symbol);
  b += field(54, "1");       // Side Buy
  b += field(40, "2");       // OrdType Limit
  b += field(38, "10");      // OrderQty
  b += field(44, "100.25");  // Price
  for (const auto& [tag, val] : extra)
  {
    b += field(tag, val);
  }
  return b;
}

const NewOrder* asNewOrder(const std::optional<InboundCommand>& cmd)
{
  return cmd.has_value() ? std::get_if<NewOrder>(&*cmd) : nullptr;
}

// ---- TimeInForce (59) ----

// Control: the three mappings the codec makes today and must keep making.
TEST(FixCodecTimeInForce, KnownValuesKeepDecodingAsTheyDo)
{
  std::string reason;

  const auto absent = decodeWithReason(newOrder("1"), reason);
  ASSERT_NE(asNewOrder(absent), nullptr) << reason;
  EXPECT_EQ(asNewOrder(absent)->tif, TimeInForce::GTC);
  EXPECT_EQ(asNewOrder(absent)->expiryNs.raw(), 0);

  const auto gtc = decodeWithReason(newOrder("2", {{59, "1"}}), reason);
  ASSERT_NE(asNewOrder(gtc), nullptr) << reason;
  EXPECT_EQ(asNewOrder(gtc)->tif, TimeInForce::GTC);

  const auto ioc = decodeWithReason(newOrder("3", {{59, "3"}}), reason);
  ASSERT_NE(asNewOrder(ioc), nullptr) << reason;
  EXPECT_EQ(asNewOrder(ioc)->tif, TimeInForce::IOC);

  const auto fok = decodeWithReason(newOrder("4", {{59, "4"}}), reason);
  ASSERT_NE(asNewOrder(fok), nullptr) << reason;
  EXPECT_EQ(asNewOrder(fok)->tif, TimeInForce::FOK);
}

TEST(FixCodecTimeInForce, UnknownValueRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(newOrder("10", {{59, "9"}}), reason);
  EXPECT_FALSE(cmd.has_value())
      << "59=9 names no FIX TimeInForce; it decoded to tif="
      << static_cast<int>(asNewOrder(cmd)->tif);
  EXPECT_TRUE(reasonNames(reason, "TimeInForce", "59"));
}

TEST(FixCodecTimeInForce, NonNumericValueRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(newOrder("11", {{59, "X"}}), reason);
  EXPECT_FALSE(cmd.has_value())
      << "59=X is not a TimeInForce at all; it decoded to tif="
      << static_cast<int>(asNewOrder(cmd)->tif);
  EXPECT_TRUE(reasonNames(reason, "TimeInForce", "59"));
}

// 59=2 AtTheOpening and 59=7 AtTheClose are real FIX 4.4 values this venue
// has no session schedule to honour. Accepting them as GTC rests an order
// the sender asked to live for one auction.
TEST(FixCodecTimeInForce, UnsupportedFixValuesRefusedRatherThanRestedAsGtc)
{
  for (const char* v : {"2", "7"})
  {
    std::string reason;
    const auto cmd = decodeWithReason(newOrder("12", {{59, v}}), reason);
    EXPECT_FALSE(cmd.has_value()) << "59=" << v << " decoded to tif="
                                  << static_cast<int>(asNewOrder(cmd)->tif);
    EXPECT_TRUE(reasonNames(reason, "TimeInForce", "59")) << "59=" << v;
  }
}

// 59=6 GTD with 126 ExpireTime. 20260925-12:00:00.000 UTC is
// 1790337600000000000 ns since the epoch -- the value FixSession::sendingTime
// (fix_wire.h:126, the inverse of this parse) renders back to that string.
TEST(FixCodecTimeInForce, GtdWithExpireTimeDecodesToGtdWithTheExpiry)
{
  std::string reason;
  const auto cmd =
      decodeWithReason(newOrder("20", {{59, "6"}, {126, "20260925-12:00:00.000"}}), reason);
  const NewOrder* o = asNewOrder(cmd);
  ASSERT_NE(o, nullptr) << "GTD over FIX refused: " << reason;
  EXPECT_EQ(o->tif, TimeInForce::GTD);
  EXPECT_EQ(o->expiryNs.raw(), 1790337600000000000LL);
  EXPECT_EQ(FixSession::sendingTime(o->expiryNs.raw()), "20260925-12:00:00.000");
}

TEST(FixCodecTimeInForce, GtdExpireTimeKeepsItsMilliseconds)
{
  std::string reason;
  const auto cmd =
      decodeWithReason(newOrder("21", {{59, "6"}, {126, "20260925-12:00:00.250"}}), reason);
  const NewOrder* o = asNewOrder(cmd);
  ASSERT_NE(o, nullptr) << "GTD over FIX refused: " << reason;
  EXPECT_EQ(o->tif, TimeInForce::GTD);
  EXPECT_EQ(o->expiryNs.raw(), 1790337600250000000LL);
}

// GTD is the one TimeInForce that carries a second required field. Without
// 126 there is no date to be good till, and today the order rests as a GTC
// that never expires.
TEST(FixCodecTimeInForce, GtdWithoutExpireTimeRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(newOrder("22", {{59, "6"}}), reason);
  EXPECT_FALSE(cmd.has_value())
      << "59=6 with no 126 decoded to tif=" << static_cast<int>(asNewOrder(cmd)->tif)
      << " expiryNs=" << asNewOrder(cmd)->expiryNs.raw();
  EXPECT_TRUE(reasonNames(reason, "ExpireTime", "126"));
}

TEST(FixCodecTimeInForce, GtdWithAMalformedExpireTimeRefusedWithAReason)
{
  for (const char* v : {"not-a-time", "20260925", "20261325-12:00:00.000", ""})
  {
    std::string reason;
    const auto cmd = decodeWithReason(newOrder("23", {{59, "6"}, {126, v}}), reason);
    EXPECT_FALSE(cmd.has_value()) << "126=" << v << " decoded to expiryNs="
                                  << asNewOrder(cmd)->expiryNs.raw();
    EXPECT_TRUE(reasonNames(reason, "ExpireTime", "126")) << "126=" << v;
  }
}

// ---- Side (54) ----

// 35=D with every field a valid order needs EXCEPT Side. Built here rather
// than through `newOrder`, which always writes one: the overrides that helper
// takes can replace a field, not remove it.
std::string newOrderWithoutSide(const std::string& clOrdId)
{
  std::string b = field(35, "D");
  b += field(11, clOrdId);
  b += field(1, std::to_string(ACCT));
  b += field(55, std::to_string(SYM));
  b += field(40, "2");
  b += field(38, "10");
  b += field(44, "100.25");
  return b;
}

// Side is the oldest of the codec's strict fields -- fix_codec.h:13-17 has
// named it since before TimeInForce joined it -- and the reason it refuses
// has to travel the same way the new ones do. A refusal that returns
// std::nullopt without setting *reason leaves the session with nothing but
// "malformed" to put in Text (58), which is the state this whole file exists
// to end.
TEST(FixCodecSide, MissingSideRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(newOrderWithoutSide("60"), reason);
  EXPECT_FALSE(cmd.has_value()) << "35=D with no 54 decoded to side "
                                << static_cast<int>(asNewOrder(cmd)->side);
  EXPECT_TRUE(reasonNames(reason, "Side", "54"));
}

TEST(FixCodecSide, InvalidSideRefusedWithAReason)
{
  // 54=3 is Buy minus (a real FIX 4.4 value this venue does not run), 54=B
  // and 54=1X name nothing, 54= is present and empty. None may become BUY.
  for (const char* v : {"3", "B", "1X", "", "0", "-1"})
  {
    std::string reason;
    const auto cmd = decodeWithReason(newOrder("61", {{54, v}}), reason);
    EXPECT_FALSE(cmd.has_value()) << "54=\"" << v << "\" decoded to side "
                                  << static_cast<int>(asNewOrder(cmd)->side);
    EXPECT_TRUE(reasonNames(reason, "Side", "54")) << "54=\"" << v << "\"";
  }
}

// Control: the two values it does run still decode, and to different sides.
TEST(FixCodecSide, BuyAndSellStillDecode)
{
  std::string reason;
  const auto buy = decodeWithReason(newOrder("62", {{54, "1"}}), reason);
  ASSERT_NE(asNewOrder(buy), nullptr) << reason;
  EXPECT_EQ(asNewOrder(buy)->side, Side::BUY);

  const auto sell = decodeWithReason(newOrder("63", {{54, "2"}}), reason);
  ASSERT_NE(asNewOrder(sell), nullptr) << reason;
  EXPECT_EQ(asNewOrder(sell)->side, Side::SELL);
}

// ---- ClOrdID (11), OrigClOrdID (41), Account (1), Symbol (55) ----

// Control: a numeric value round-trips exactly, including the 20-digit
// boundary that still fits uint64 (UINT64_MAX).
TEST(FixCodecStringFields, NumericValuesRoundTripExactly)
{
  std::string reason;

  const auto small = decodeWithReason(newOrder("42", {}, "7", "1"), reason);
  ASSERT_NE(asNewOrder(small), nullptr) << reason;
  EXPECT_EQ(asNewOrder(small)->id, 42ULL);
  EXPECT_EQ(asNewOrder(small)->clientOrderId, 42ULL);
  EXPECT_EQ(asNewOrder(small)->accountId, 7ULL);
  EXPECT_EQ(asNewOrder(small)->symbol, 1U);

  // 20 digits, exactly UINT64_MAX: the largest ClOrdID this venue can carry.
  const auto big = decodeWithReason(newOrder("18446744073709551615", {}, "18446744073709551615",
                                             "4294967295"),
                                    reason);
  ASSERT_NE(asNewOrder(big), nullptr) << reason;
  EXPECT_EQ(asNewOrder(big)->id, 18446744073709551615ULL);
  EXPECT_EQ(asNewOrder(big)->clientOrderId, 18446744073709551615ULL);
  EXPECT_EQ(asNewOrder(big)->accountId, 18446744073709551615ULL);
  EXPECT_EQ(asNewOrder(big)->symbol, 4294967295U);
}

// FIX 4.4 types ClOrdID as String. An alphanumeric one is not an error on
// the wire -- it is an id this venue cannot carry -- and the sender has to be
// told so, because today both of these become venue order id 0.
TEST(FixCodecStringFields, AlphanumericClOrdIdRefusedWithAReason)
{
  std::string reasonA;
  std::string reasonB;
  const auto a = decodeWithReason(newOrder("ORD-A1"), reasonA);
  const auto b = decodeWithReason(newOrder("ORD-B2"), reasonB);

  EXPECT_FALSE(a.has_value()) << "11=ORD-A1 decoded to id " << asNewOrder(a)->id;
  EXPECT_TRUE(reasonNames(reasonA, "ClOrdID", "11"));
  EXPECT_FALSE(b.has_value()) << "11=ORD-B2 decoded to id " << asNewOrder(b)->id;
  EXPECT_TRUE(reasonNames(reasonB, "ClOrdID", "11"));

  // The failure this refusal exists to prevent: two clients, two different
  // names, one venue order id.
  if (a.has_value() && b.has_value())
  {
    EXPECT_NE(asNewOrder(a)->id, asNewOrder(b)->id)
        << "two different ClOrdIDs share one venue order id";
  }
}

TEST(FixCodecStringFields, PartiallyNumericClOrdIdRefusedWithAReason)
{
  for (const char* v : {"123ABC", " 42", "+42", "0x10", "4.2", ""})
  {
    std::string reason;
    const auto cmd = decodeWithReason(newOrder(v), reason);
    EXPECT_FALSE(cmd.has_value()) << "11=\"" << v << "\" decoded to id " << asNewOrder(cmd)->id;
    EXPECT_TRUE(reasonNames(reason, "ClOrdID", "11")) << "11=\"" << v << "\"";
  }
}

// strtoull accepts a leading '-' and wraps: 11=-1 lands on UINT64_MAX, the
// same id a client that legitimately named UINT64_MAX gets.
TEST(FixCodecStringFields, NegativeClOrdIdRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(newOrder("-1"), reason);
  EXPECT_FALSE(cmd.has_value()) << "11=-1 decoded to id " << asNewOrder(cmd)->id;
  EXPECT_TRUE(reasonNames(reason, "ClOrdID", "11"));
}

// The other side of the 20-digit boundary: 99999999999999999999 does not fit
// uint64. strtoull saturates to UINT64_MAX and sets ERANGE, which nobody
// reads, so this collides with the legitimate UINT64_MAX above.
TEST(FixCodecStringFields, OverflowingClOrdIdRefusedWithAReason)
{
  std::string reason;
  const auto over = decodeWithReason(newOrder("99999999999999999999"), reason);
  EXPECT_FALSE(over.has_value()) << "11=99999999999999999999 decoded to id "
                                 << asNewOrder(over)->id;
  EXPECT_TRUE(reasonNames(reason, "ClOrdID", "11"));

  if (over.has_value())
  {
    std::string maxReason;
    const auto max = decodeWithReason(newOrder("18446744073709551615"), maxReason);
    ASSERT_NE(asNewOrder(max), nullptr) << maxReason;
    EXPECT_NE(asNewOrder(over)->id, asNewOrder(max)->id)
        << "an overflowing ClOrdID lands on the same id as UINT64_MAX";
  }
}

TEST(FixCodecStringFields, NonNumericAccountRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(newOrder("50", {}, "ACME"), reason);
  EXPECT_FALSE(cmd.has_value()) << "1=ACME decoded to accountId " << asNewOrder(cmd)->accountId;
  EXPECT_TRUE(reasonNames(reason, "Account", "1"));
}

TEST(FixCodecStringFields, NonNumericSymbolRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(newOrder("51", {}, std::to_string(ACCT), "BTC-USD"), reason);
  EXPECT_FALSE(cmd.has_value()) << "55=BTC-USD decoded to symbol " << asNewOrder(cmd)->symbol;
  EXPECT_TRUE(reasonNames(reason, "Symbol", "55"));
}

// SymbolId is uint32: strtoul returns 64 bits on this platform and the cast
// truncates, so 55=4294967296 routes to symbol 0 -- someone else's book.
TEST(FixCodecStringFields, SymbolAboveUint32RefusedWithAReason)
{
  std::string reason;
  const auto cmd =
      decodeWithReason(newOrder("52", {}, std::to_string(ACCT), "4294967296"), reason);
  EXPECT_FALSE(cmd.has_value()) << "55=4294967296 decoded to symbol " << asNewOrder(cmd)->symbol;
  EXPECT_TRUE(reasonNames(reason, "Symbol", "55"));
}

// OrderCancelRequest (35=F) and OrderCancelReplaceRequest (35=G) bodies.
// `extra` is appended after the fields above and parseFields keeps the LAST
// occurrence of a tag, so an entry there replaces the default rather than
// adding a second copy -- the same way `newOrder` above takes its overrides.
std::string cancelRequest(const std::string& origClOrdId,
                          const std::string& account = std::to_string(ACCT),
                          const std::vector<std::pair<int, std::string>>& extra = {})
{
  std::string b = field(35, "F");
  b += field(41, origClOrdId);
  b += field(1, account);
  b += field(55, std::to_string(SYM));
  for (const auto& [tag, val] : extra)
  {
    b += field(tag, val);
  }
  return b;
}

std::string cancelReplace(const std::string& origClOrdId,
                          const std::vector<std::pair<int, std::string>>& extra = {})
{
  std::string b = field(35, "G");
  b += field(41, origClOrdId);
  b += field(1, std::to_string(ACCT));
  b += field(55, std::to_string(SYM));
  b += field(38, "5");
  b += field(44, "100.25");
  for (const auto& [tag, val] : extra)
  {
    b += field(tag, val);
  }
  return b;
}

// Control: F and G keep decoding a numeric OrigClOrdID unchanged.
TEST(FixCodecStringFields, NumericOrigClOrdIdRoundTripsOnCancelAndReplace)
{
  std::string reason;

  const auto f = decodeWithReason(cancelRequest("77"), reason);
  ASSERT_TRUE(f.has_value()) << reason;
  const auto* c = std::get_if<CancelOrder>(&*f);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->id, 77ULL);
  EXPECT_EQ(c->accountId, ACCT);
  EXPECT_EQ(c->symbol, SYM);

  const auto g = decodeWithReason(cancelReplace("18446744073709551615"), reason);
  ASSERT_TRUE(g.has_value()) << reason;
  const auto* m = std::get_if<ModifyOrder>(&*g);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->id, 18446744073709551615ULL);
}

TEST(FixCodecStringFields, NonNumericOrigClOrdIdRefusedWithAReason)
{
  for (const char* v : {"CANCEL-ME", "-1", "99999999999999999999", "7X"})
  {
    std::string fReason;
    const auto f = decodeWithReason(cancelRequest(v), fReason);
    EXPECT_FALSE(f.has_value())
        << "35=F 41=" << v << " decoded to id " << std::get_if<CancelOrder>(&*f)->id;
    EXPECT_TRUE(reasonNames(fReason, "OrigClOrdID", "41")) << "35=F 41=" << v;

    std::string gReason;
    const auto g = decodeWithReason(cancelReplace(v), gReason);
    EXPECT_FALSE(g.has_value())
        << "35=G 41=" << v << " decoded to id " << std::get_if<ModifyOrder>(&*g)->id;
    EXPECT_TRUE(reasonNames(gReason, "OrigClOrdID", "41")) << "35=G 41=" << v;
  }
}

TEST(FixCodecStringFields, NonNumericAccountOnCancelRefusedWithAReason)
{
  std::string reason;
  const auto cmd = decodeWithReason(cancelRequest("77", "ACME"), reason);
  EXPECT_FALSE(cmd.has_value()) << "35=F 1=ACME decoded to accountId "
                                << std::get_if<CancelOrder>(&*cmd)->accountId;
  EXPECT_TRUE(reasonNames(reason, "Account", "1"));
}

// A cancel/replace carries the account that is billed and the symbol whose
// book is touched, and they are two different fields of ModifyOrder. Distinct
// values on purpose: with 1 and 55 folded into one number, or written into
// each other's field, this reads the same either way.
TEST(FixCodecStringFields, CancelReplaceCarriesAccountAndSymbolIntoTheirOwnFields)
{
  std::string reason;
  const auto g = decodeWithReason(cancelReplace("90", {{1, "4242"}, {55, "77"}}), reason);
  ASSERT_TRUE(g.has_value()) << reason;
  const auto* m = std::get_if<ModifyOrder>(&*g);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->id, 90ULL);
  EXPECT_EQ(m->accountId, 4242ULL);
  EXPECT_EQ(m->symbol, 77U);

  // Same claim at the widths of both fields, where a swap would not even fit.
  const auto wide =
      decodeWithReason(cancelReplace("91", {{1, "18446744073709551615"}, {55, "4294967295"}}),
                       reason);
  ASSERT_TRUE(wide.has_value()) << reason;
  const auto* w = std::get_if<ModifyOrder>(&*wide);
  ASSERT_NE(w, nullptr);
  EXPECT_EQ(w->accountId, 18446744073709551615ULL);
  EXPECT_EQ(w->symbol, 4294967295U);
}

// Price (44) on a replace is optional -- an amend may resize without
// repricing -- but a value that IS present and does not parse must refuse,
// not fall through to "keep the current price". A maker who sent a price
// meant to move the order; leaving it where it was is the one outcome nobody
// asked for.
TEST(FixCodecStringFields, CancelReplaceWithAMalformedPriceRefusedWithAReason)
{
  for (const char* v : {"abc", "1e2", "100.25.1", "", "1,25", "-"})
  {
    std::string reason;
    const auto g = decodeWithReason(cancelReplace("92", {{44, v}}), reason);
    EXPECT_FALSE(g.has_value()) << "35=G 44=\"" << v << "\" decoded to newPrice "
                                << std::get_if<ModifyOrder>(&*g)->newPrice.raw();
    EXPECT_TRUE(reasonNames(reason, "Price", "44")) << "35=G 44=\"" << v << "\"";
  }
}

// Control: a well-formed one round-trips to the raw it names, and an absent
// one leaves newPrice at 0 (the "keep the current price" the engine reads).
TEST(FixCodecStringFields, CancelReplacePriceRoundTrips)
{
  std::string reason;
  const auto g = decodeWithReason(cancelReplace("93", {{44, "100.25"}}), reason);
  ASSERT_TRUE(g.has_value()) << reason;
  const auto* m = std::get_if<ModifyOrder>(&*g);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->newPrice.raw(), Price::fromDouble(100.25).raw());
  EXPECT_EQ(m->newQty.raw(), Quantity::fromDouble(5.0).raw());

  std::string b = field(35, "G");
  b += field(41, "94");
  b += field(1, std::to_string(ACCT));
  b += field(55, std::to_string(SYM));
  b += field(38, "5");
  const auto noPrice = decodeWithReason(b, reason);
  ASSERT_TRUE(noPrice.has_value()) << reason;
  EXPECT_EQ(std::get_if<ModifyOrder>(&*noPrice)->newPrice.raw(), 0);
}

// Symbol (55) on a cancel names the book the order rests in. Truncated to 0
// it cancels in someone else's, which is worse than a refusal the sender can
// read -- the same reason 35=D refuses it.
TEST(FixCodecStringFields, CancelRequestWithAMalformedSymbolRefusedWithAReason)
{
  for (const char* v : {"BTC-USD", "4294967296", "-1", "1 ", "01", ""})
  {
    std::string reason;
    const auto f = decodeWithReason(cancelRequest("95", std::to_string(ACCT), {{55, v}}), reason);
    EXPECT_FALSE(f.has_value()) << "35=F 55=\"" << v << "\" decoded to symbol "
                                << std::get_if<CancelOrder>(&*f)->symbol;
    EXPECT_TRUE(reasonNames(reason, "Symbol", "55")) << "35=F 55=\"" << v << "\"";
  }
}

// ---- the quoting order-id block ----
//
// docs/venue/fix-quoting.md, "The order-id block": "The venue therefore
// derives bidIdBase/askIdBase deterministically from (accountId, symbol)
// (FixCodec::quoteLadderIdBase): every MassQuote or QuoteCancel from one
// account on one symbol addresses the same 2*kQuoteLadderLevels-id block,
// bid block then ask block. That is what lets a QuoteCancel -- which names
// no ids of its own -- take down exactly the legs the last MassQuote on that
// account/symbol put up".
//
// "exactly the legs the last MassQuote on THAT account/symbol put up" is a
// statement about two directions at once: one pair always reaches its own
// block (the control below), and no other pair reaches it. The fold
// account*4099 + symbol only delivers the first: fix_codec.h:109 claims "two
// different (account, symbol) pairs land in different blocks" and (1, 4099)
// and (2, 0) both fold to 8198.

constexpr uint64_t kBlock = 2ULL * kQuoteLadderLevels;

// Control: the same pair is stable, and a MassQuote and a QuoteCancel from
// one account on one symbol address the same block. This is the half of the
// contract that works today and has to keep working.
// QuoteCancel (35=Z) and a one-level MassQuote (35=i), the two messages the
// id block is derived inside.
std::string quoteCancelMsg(const std::string& account, const std::string& symbol)
{
  std::string b = field(35, "Z");
  b += field(1, account);
  b += field(55, symbol);
  b += field(298, "4");  // QuoteCancelType: all
  return b;
}

std::string massQuoteMsg(const std::string& account, const std::string& symbol)
{
  std::string b = field(35, "i");
  b += field(1, account);
  b += field(117, "555");
  b += field(299, "1000");
  b += field(55, symbol);
  b += field(132, "99.99");
  b += field(133, "100.01");
  b += field(134, "1");
  b += field(135, "2");
  return b;
}

// docs/venue/fix-quoting.md, "The range": "| `Account` (1) | 24 bits |
// `0 .. 16777215` (`FixCodec::kQuoteAccountLimit - 1`) |", and below it: "An
// `Account` above the range is **refused** naming `Account(1)`, on MassQuote
// and QuoteCancel alike".
//
// The number is written out here rather than read back off the constant. A
// test that says kQuoteAccountLimit == kQuoteAccountLimit passes for every
// value the constant could be given, including one the documentation does
// not name -- and the range is not an implementation detail a maker can
// discover: it is the number an operator hands out account ids against.
TEST(FixCodecQuoteLadderIdBlock, TheAccountRangeIsTheNumberTheDocumentationPrints)
{
  EXPECT_EQ(FixCodec::kQuoteAccountLimit, 16777216ULL);
  EXPECT_EQ(FixCodec::kQuoteAccountLimit, 1ULL << 24);
  EXPECT_TRUE(FixCodec::quoteIdBlockInRange(16777215ULL));
  EXPECT_FALSE(FixCodec::quoteIdBlockInRange(16777216ULL));

  // The boundary through decode(), on both messages that derive a block: the
  // last account in range quotes, the first one out of it is refused naming
  // the field.
  for (const auto& [what, msg] :
       std::vector<std::pair<const char*, std::string>>{
           {"35=Z", quoteCancelMsg("16777215", std::to_string(SYM))},
           {"35=i", massQuoteMsg("16777215", std::to_string(SYM))}})
  {
    std::string reason;
    const auto cmd = decodeWithReason(msg, reason);
    ASSERT_TRUE(cmd.has_value()) << what << " account 16777215 refused: " << reason;
    const auto* l = std::get_if<QuoteLadder>(&*cmd);
    ASSERT_NE(l, nullptr) << what;
    EXPECT_EQ(l->accountId, 16777215ULL) << what;
    EXPECT_EQ(l->bidIdBase, FixCodec::quoteLadderIdBase(16777215ULL, SYM)) << what;
  }

  for (const auto& [what, msg] :
       std::vector<std::pair<const char*, std::string>>{
           {"35=Z", quoteCancelMsg("16777216", std::to_string(SYM))},
           {"35=i", massQuoteMsg("16777216", std::to_string(SYM))}})
  {
    std::string reason;
    const auto cmd = decodeWithReason(msg, reason);
    EXPECT_FALSE(cmd.has_value())
        << what << " account 16777216 decoded to block base "
        << std::get_if<QuoteLadder>(&*cmd)->bidIdBase;
    EXPECT_TRUE(reasonNames(reason, "Account", "1")) << what;
  }
}

TEST(FixCodecQuoteLadderIdBlock, OnePairAlwaysReachesItsOwnBlock)
{
  EXPECT_EQ(FixCodec::quoteLadderIdBase(ACCT, SYM), FixCodec::quoteLadderIdBase(ACCT, SYM));

  std::string b = field(35, "Z");
  b += field(1, std::to_string(ACCT));
  b += field(55, std::to_string(SYM));
  std::string reason;
  const auto cmd = decodeWithReason(b, reason);
  ASSERT_TRUE(cmd.has_value()) << reason;
  const auto* l = std::get_if<QuoteLadder>(&*cmd);
  ASSERT_NE(l, nullptr);
  EXPECT_EQ(l->bidIdBase, FixCodec::quoteLadderIdBase(ACCT, SYM));
  EXPECT_EQ(l->askIdBase, l->bidIdBase + kQuoteLadderLevels);
}

// The literal collision: account 1 quoting symbol 4099 and account 2 quoting
// symbol 0 are handed the same 16 ids, so either one's QuoteCancel takes the
// other's ladder down.
TEST(FixCodecQuoteLadderIdBlock, TwoDifferentPairsDoNotShareABlock)
{
  EXPECT_NE(FixCodec::quoteLadderIdBase(1, 4099), FixCodec::quoteLadderIdBase(2, 0));
  EXPECT_NE(FixCodec::quoteLadderIdBase(1, 8198), FixCodec::quoteLadderIdBase(3, 0));
  EXPECT_NE(FixCodec::quoteLadderIdBase(5, 4099), FixCodec::quoteLadderIdBase(6, 0));
}

// Every pair in an ordinary operating range gets a block of its own, not just
// the three pairs named above: a fold with a bigger multiplier moves the
// collision, it does not remove it.
TEST(FixCodecQuoteLadderIdBlock, NoTwoPairsInTheOperatingRangeShareABlock)
{
  std::unordered_set<OrderId> seen;
  seen.reserve(128 * 8192 * 2);
  size_t pairs = 0;
  for (uint64_t account = 0; account < 128; ++account)
  {
    for (SymbolId symbol = 0; symbol < 8192; ++symbol)
    {
      ++pairs;
      const OrderId base = FixCodec::quoteLadderIdBase(account, symbol);
      if (!seen.insert(base).second)
      {
        FAIL() << "account " << account << " symbol " << symbol << " reuses block base " << base
               << " (" << seen.size() << " distinct bases over " << pairs << " pairs)";
      }
    }
  }
  EXPECT_EQ(seen.size(), pairs);
}

// Distinct bases are not enough: the block is 2*kQuoteLadderLevels ids wide,
// and two bases less than that apart overlap on real legs.
TEST(FixCodecQuoteLadderIdBlock, BlocksDoNotOverlap)
{
  std::unordered_set<OrderId> ids;
  for (uint64_t account = 0; account < 16; ++account)
  {
    for (SymbolId symbol = 0; symbol < 256; ++symbol)
    {
      const OrderId base = FixCodec::quoteLadderIdBase(account, symbol);
      for (uint64_t i = 0; i < kBlock; ++i)
      {
        if (!ids.insert(base + i).second)
        {
          FAIL() << "account " << account << " symbol " << symbol << " leg " << i << " (id "
                 << (base + i) << ") belongs to another pair's block";
        }
      }
    }
  }
  EXPECT_EQ(ids.size(), 16U * 256U * kBlock);
}

// Same claim through decode(), across values that reach the edges of both
// fields: a QuoteCancel this codec accepts must address a block no other
// accepted pair addresses. A pair the venue cannot carry is refused with a
// reason instead -- the codec is allowed to bound the range it folds, it is
// not allowed to fold two senders onto one ladder.
TEST(FixCodecQuoteLadderIdBlock, AcceptedQuoteCancelsNeverAddressAnotherPairsBlock)
{
  const uint64_t accounts[] = {0, 1, 2, 3, 7, 4099, 1000000, 18446744073709551615ULL};
  const uint64_t symbols[] = {0, 1, 4099, 8198, 65535, 65536, 1000000, 2147483648ULL, 4294967295ULL};

  std::unordered_set<OrderId> ids;
  size_t accepted = 0;
  for (uint64_t account : accounts)
  {
    for (uint64_t symbol : symbols)
    {
      std::string b = field(35, "Z");
      b += field(1, std::to_string(account));
      b += field(55, std::to_string(symbol));
      std::string reason;
      const auto cmd = decodeWithReason(b, reason);
      if (!cmd.has_value())
      {
        EXPECT_FALSE(reason.empty())
            << "account " << account << " symbol " << symbol << " refused with no reason";
        continue;
      }
      const auto* l = std::get_if<QuoteLadder>(&*cmd);
      ASSERT_NE(l, nullptr);
      ++accepted;
      for (uint64_t i = 0; i < kBlock; ++i)
      {
        EXPECT_TRUE(ids.insert(l->bidIdBase + i).second)
            << "account " << account << " symbol " << symbol << " leg " << i << " (id "
            << (l->bidIdBase + i) << ") belongs to another pair's block";
      }
    }
  }
  EXPECT_GT(accepted, 0U) << "every pair was refused: the codec quotes for nobody";
}

// ---- the two overloads are one decoder ----
//
// decode(msg) is the hook the gateways install (tcp_gateway.h, and the
// FixVenue in test_venue_fix_mass_quote.cpp), decode(msg, &reason) is what
// the FIX session layer calls so it can answer with the field. They are the
// same decoder or they are a hole: a strictness that only applies when
// somebody passes a place to write the reason is a strictness every gateway
// on this venue is missing.

// Every malformed message this file refuses, in one place.
std::vector<std::pair<const char*, std::string>> malformedCorpus()
{
  return {
      {"59=9", newOrder("1", {{59, "9"}})},
      {"59=X", newOrder("1", {{59, "X"}})},
      {"59=2", newOrder("1", {{59, "2"}})},
      {"59=7", newOrder("1", {{59, "7"}})},
      {"59=6 no 126", newOrder("1", {{59, "6"}})},
      {"59=6 bad 126", newOrder("1", {{59, "6"}, {126, "not-a-time"}})},
      {"59=6 impossible 126", newOrder("1", {{59, "6"}, {126, "20260229-12:00:00.000"}})},
      {"no 54", newOrderWithoutSide("1")},
      {"54=3", newOrder("1", {{54, "3"}})},
      {"54 empty", newOrder("1", {{54, ""}})},
      {"40=9", newOrder("1", {{40, "9"}})},
      {"11=ORD-A1", newOrder("ORD-A1")},
      {"11=123ABC", newOrder("123ABC")},
      {"11= 42", newOrder(" 42")},
      {"11=+42", newOrder("+42")},
      {"11=-1", newOrder("-1")},
      {"11 overflow", newOrder("99999999999999999999")},
      {"11 empty", newOrder("")},
      {"1=ACME", newOrder("1", {}, "ACME")},
      {"55=BTC-USD", newOrder("1", {}, std::to_string(ACCT), "BTC-USD")},
      {"55=4294967296", newOrder("1", {}, std::to_string(ACCT), "4294967296")},
      {"38 missing", []
       {
         std::string b = field(35, "D");
         b += field(11, "1");
         b += field(54, "1");
         return b;
       }()},
      {"35=F 41=CANCEL-ME", cancelRequest("CANCEL-ME")},
      {"35=F 41 overflow", cancelRequest("99999999999999999999")},
      {"35=F 1=ACME", cancelRequest("77", "ACME")},
      {"35=F 55=BTC-USD", cancelRequest("77", std::to_string(ACCT), {{55, "BTC-USD"}})},
      {"35=G 41=-1", cancelReplace("-1")},
      {"35=G 44=abc", cancelReplace("90", {{44, "abc"}})},
      {"35=G 55=4294967296", cancelReplace("90", {{55, "4294967296"}})},
      {"35=Z 1=ACME", quoteCancelMsg("ACME", std::to_string(SYM))},
      {"35=Z account out of range", quoteCancelMsg("16777216", std::to_string(SYM))},
      {"35=Z 55=BTC-USD", quoteCancelMsg(std::to_string(ACCT), "BTC-USD")},
      {"35=i account out of range", massQuoteMsg("16777216", std::to_string(SYM))},
      {"35=W", field(35, "W")},
  };
}

std::vector<std::pair<const char*, std::string>> acceptedCorpus()
{
  return {
      {"plain order", newOrder("1")},
      {"59 absent", newOrder("2")},
      {"59=1", newOrder("3", {{59, "1"}})},
      {"59=3", newOrder("4", {{59, "3"}})},
      {"59=4", newOrder("5", {{59, "4"}})},
      {"59=6 with 126", newOrder("6", {{59, "6"}, {126, "20260925-12:00:00.000"}})},
      {"54=2", newOrder("7", {{54, "2"}})},
      {"11 at UINT64_MAX", newOrder("18446744073709551615")},
      {"35=F", cancelRequest("77")},
      {"35=G", cancelReplace("78")},
      {"35=Z", quoteCancelMsg(std::to_string(ACCT), std::to_string(SYM))},
      {"35=i", massQuoteMsg(std::to_string(ACCT), std::to_string(SYM))},
      {"35=Z at the top of the account range",
       quoteCancelMsg("16777215", std::to_string(SYM))},
  };
}

TEST(FixCodecOverloads, TheOneArgumentDecodeRefusesEverythingTheTwoArgumentOneDoes)
{
  for (const auto& [what, msg] : malformedCorpus())
  {
    std::string reason;
    const auto withReason = FixCodec::decode(msg, &reason);
    const auto bare = FixCodec::decode(msg);
    EXPECT_FALSE(withReason.has_value()) << what;
    EXPECT_FALSE(reason.empty()) << what << ": refused with no reason";
    EXPECT_FALSE(bare.has_value())
        << what << ": decode(msg) accepted a message decode(msg, &reason) refused as \"" << reason
        << "\" -- the gateways install the one-argument overload";
  }
}

TEST(FixCodecOverloads, TheTwoOverloadsAgreeOnEveryMessageTheyAccept)
{
  for (const auto& [what, msg] : acceptedCorpus())
  {
    std::string reason;
    const auto withReason = FixCodec::decode(msg, &reason);
    const auto bare = FixCodec::decode(msg);
    ASSERT_TRUE(withReason.has_value()) << what << ": " << reason;
    ASSERT_TRUE(bare.has_value()) << what;
    EXPECT_TRUE(reason.empty()) << what << ": accepted with a reason \"" << reason << "\"";
    ASSERT_EQ(withReason->index(), bare->index()) << what;

    if (const auto* a = std::get_if<NewOrder>(&*withReason))
    {
      const auto* b = std::get_if<NewOrder>(&*bare);
      ASSERT_NE(b, nullptr) << what;
      EXPECT_EQ(a->id, b->id) << what;
      EXPECT_EQ(a->side, b->side) << what;
      EXPECT_EQ(a->tif, b->tif) << what;
      EXPECT_EQ(a->expiryNs.raw(), b->expiryNs.raw()) << what;
    }
    if (const auto* a = std::get_if<QuoteLadder>(&*withReason))
    {
      const auto* b = std::get_if<QuoteLadder>(&*bare);
      ASSERT_NE(b, nullptr) << what;
      EXPECT_EQ(a->bidIdBase, b->bidIdBase) << what;
      EXPECT_EQ(a->askIdBase, b->askIdBase) << what;
    }
  }
}

}  // namespace
