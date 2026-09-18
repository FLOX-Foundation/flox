/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Every reject reason has a name, and the metrics array is as long as the
 * enum.
 *
 * These two drift apart silently. A reason appended past Metrics::kReasons is
 * not merely miscounted -- the per-reason counter drops it (observe() checks
 * the bound), so the rejects appear in the total and in no breakdown, and the
 * dashboard looks healthy while an entire refusal class is invisible. A reason
 * appended without a text comes out of toString as "?", which reaches a client
 * as the contents of FIX tag 58.
 *
 * Neither failure announces itself, so both are asked about here.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/reject_reason.h"
#include "flox-venue/sbe_order_entry_codec.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

using namespace flox;
using namespace flox::venue;

// The array ends exactly where the enum does. Asked of toString rather than of
// a second number: a reason that has a name but no slot is the bug.
TEST(RejectReasons, TheMetricsArrayIsAsLongAsTheEnum)
{
  const auto name = [](size_t v)
  { return std::string(toString(static_cast<RejectReason>(v))); };

  EXPECT_NE(name(Metrics::kReasons - 1), "?")
      << "the last counted reason has no name: kReasons is past the end of the enum";
  EXPECT_EQ(name(Metrics::kReasons), "?")
      << "a reason exists past the end of the metrics array. It would be counted in the total "
         "and dropped from every per-reason series -- bump kReasons";
}

// No reason is nameless, and no two share a name: a shared name means a client
// reading tag 58 cannot tell two refusals apart.
TEST(RejectReasons, EveryReasonHasItsOwnName)
{
  std::set<std::string> seen;
  for (size_t v = 0; v < Metrics::kReasons; ++v)
  {
    const std::string n = toString(static_cast<RejectReason>(v));
    EXPECT_NE(n, "?") << "reason " << v << " has no text";
    EXPECT_TRUE(seen.insert(n).second) << "two reasons both answer " << n;
  }
}

// The two the external risk owner needs. Before these existed it had to borrow
// somebody else's answer -- InsufficientFunds for a suspended account,
// MarketClosed for an unreachable limit source -- and a client acting on that
// text tops up an account that is not short of money, or waits for a session
// that never closed.
TEST(RejectReasons, TheRiskOwnerHasReasonsOfItsOwn)
{
  EXPECT_STREQ(toString(RejectReason::CreditRefused), "CreditRefused");
  EXPECT_STREQ(toString(RejectReason::CreditSourceUnavailable), "CreditSourceUnavailable");
  EXPECT_NE(std::string(toString(RejectReason::CreditRefused)),
            std::string(toString(RejectReason::InsufficientFunds)))
      << "the borrowed answer and the honest one must not be the same text";
}

namespace
{

constexpr SymbolId SYM = 1;

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  return c;
}

NewOrder order()
{
  NewOrder o;
  o.id = 1;
  o.clientOrderId = 99;
  o.symbol = SYM;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(100.0);
  o.quantity = Quantity::fromDouble(1);
  o.accountId = 7;
  return o;
}

std::string tag(const std::string& msg, int t)
{
  const std::string key = std::to_string(t) + "=";
  size_t pos = 0;
  while ((pos = msg.find(key, pos)) != std::string::npos)
  {
    if (pos == 0 || msg[pos - 1] == '\x01')
    {
      const size_t end = msg.find('\x01', pos);
      return msg.substr(pos + key.size(), end - pos - key.size());
    }
    pos += key.size();
  }
  return {};
}

}  // namespace

// End to end: the risk owner refuses with its own reason, and the client is
// told that reason rather than a borrowed one.
TEST(RejectReasons, ARefusalByTheRiskOwnerReachesTheClientAsItself)
{
  for (const RejectReason r : {RejectReason::CreditRefused, RejectReason::CreditSourceUnavailable})
  {
    std::vector<OutboundEvent> ev;
    MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                     { ev.push_back(e); });
    eng.setCreditCheck([r](const MatchingEngine<MatchingBook>::CreditRequest&)
                       { return MatchingEngine<MatchingBook>::CreditDecision{false, r}; });
    eng.submit(InboundCommand{order()}, 1);

    const OrderRejected* j = nullptr;
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderRejected>(&e))
      {
        j = x;
      }
    }
    ASSERT_NE(j, nullptr) << "the order was not refused at all";
    EXPECT_EQ(j->reason, r) << "the engine substituted its own reason for the risk owner's";

    // FIX 58 carries the text a human reads.
    const std::string wire = FixCodec::encode(OutboundEvent{*j});
    EXPECT_EQ(tag(wire, 58), std::string(toString(r)));
    EXPECT_NE(tag(wire, 58), std::string(toString(RejectReason::InsufficientFunds)));

    // SBE carries the value, not the text: a byte that survives the trip.
    std::vector<uint8_t> buf;
    SbeOrderEntryCodec::encode(OutboundEvent{*j}, buf, /*seq=*/1);
    bool foundByte = false;
    for (uint8_t b : buf)
    {
      foundByte = foundByte || b == static_cast<uint8_t>(r);
    }
    EXPECT_TRUE(foundByte) << "the reason byte is not on the wire";
  }
}
