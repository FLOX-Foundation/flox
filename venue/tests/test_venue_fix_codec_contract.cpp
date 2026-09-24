/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The edges of the inbound FixCodec contract that the strictness suite
 * (test_venue_fix_codec_strictness.cpp) does not name: the FIX TimeInForce
 * values with no venue behind them, what ExpireTime (126) means when the
 * order is not GTD, and the boundary of the (account, symbol) range the
 * quoting id block can fold.
 *
 * Kept apart from the strictness suite so the two can be read for what they
 * are: that file is the specification, this one is the fence around it.
 */
#include "flox-venue/fix_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

std::string field(int tag, const std::string& v)
{
  return std::to_string(tag) + "=" + v + std::string(1, FixCodec::SOH);
}

std::string newOrder(const std::vector<std::pair<int, std::string>>& extra = {})
{
  std::string b = field(35, "D");
  b += field(11, "1");
  b += field(1, "7");
  b += field(55, "1");
  b += field(54, "1");
  b += field(40, "2");
  b += field(38, "10");
  b += field(44, "100.25");
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

// 0 Day, 5 GoodTillCrossing and 8 AtCrossing are FIX 4.4 values that end at a
// session boundary or an auction this venue does not run. They are refused
// for the same reason 2 and 7 are: resting them as GTC turns an order the
// sender gave a deadline into one that has none.
TEST(FixCodecContractTimeInForce, SessionBoundedValuesAreRefused)
{
  for (const char* v : {"0", "5", "8", "", " 3", "03", "3.0", "+3", "-3", "GTC"})
  {
    std::string reason;
    const auto cmd = FixCodec::decode(newOrder({{59, v}}), &reason);
    EXPECT_FALSE(cmd.has_value()) << "59=" << v << " decoded";
    EXPECT_NE(reason.find("TimeInForce"), std::string::npos) << "59=" << v << ": " << reason;
    EXPECT_NE(reason.find("59"), std::string::npos) << "59=" << v << ": " << reason;
  }
}

// ExpireTime belongs to GTD. On any other TimeInForce FIX carries no meaning
// for it, so it is ignored rather than turned into an expiry the sender did
// not ask for -- and a GTC order still rests with no deadline.
TEST(FixCodecContractTimeInForce, ExpireTimeWithoutGtdIsIgnored)
{
  std::string reason;
  const auto cmd = FixCodec::decode(newOrder({{59, "1"}, {126, "20260925-12:00:00.000"}}), &reason);
  const NewOrder* o = asNewOrder(cmd);
  ASSERT_NE(o, nullptr) << reason;
  EXPECT_EQ(o->tif, TimeInForce::GTC);
  EXPECT_EQ(o->expiryNs.raw(), 0);

  const auto junk = FixCodec::decode(newOrder({{59, "3"}, {126, "not-a-time"}}), &reason);
  const NewOrder* j = asNewOrder(junk);
  ASSERT_NE(j, nullptr) << reason;
  EXPECT_EQ(j->tif, TimeInForce::IOC);
  EXPECT_EQ(j->expiryNs.raw(), 0);
}

// An absent 59 is the FIX default and keeps decoding without an expiry.
TEST(FixCodecContractTimeInForce, GtdExpiryIsOnlySetByGtd)
{
  std::string reason;
  const auto plain = FixCodec::decode(newOrder(), &reason);
  ASSERT_NE(asNewOrder(plain), nullptr) << reason;
  EXPECT_EQ(asNewOrder(plain)->expiryNs.raw(), 0);

  const auto gtd = FixCodec::decode(newOrder({{59, "6"}, {126, "20260925-12:00:00"}}), &reason);
  ASSERT_NE(asNewOrder(gtd), nullptr) << reason;
  EXPECT_EQ(asNewOrder(gtd)->tif, TimeInForce::GTD);
  EXPECT_EQ(asNewOrder(gtd)->expiryNs.raw(), 1790337600000000000LL)
      << "the milliseconds of an ExpireTime are optional";
}

}  // namespace
