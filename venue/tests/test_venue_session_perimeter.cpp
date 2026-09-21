/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/session.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr char kSoh = '\x01';
constexpr SymbolId kSym = 11;

std::string field(int tag, const std::string& value)
{
  return std::to_string(tag) + "=" + value + kSoh;
}

// A NewOrderSingle the FIX codec accepts. No CheckSum: FixCodec::decode is
// lenient when tag 10 is absent, which keeps these frames readable.
std::string newOrderSingle(uint64_t clOrdId, uint64_t account = 0)
{
  return field(35, "D") + field(11, std::to_string(clOrdId)) + field(55, std::to_string(kSym)) +
         field(1, std::to_string(account)) + field(54, "1") + field(38, "1") + field(40, "2") +
         field(44, "100");
}

std::unordered_map<int, std::string> fields(const std::string& msg)
{
  std::unordered_map<int, std::string> out;
  size_t i = 0;
  while (i < msg.size())
  {
    const size_t end = msg.find(kSoh, i);
    const std::string kv = msg.substr(i, end == std::string::npos ? std::string::npos : end - i);
    const size_t eq = kv.find('=');
    if (eq != std::string::npos)
    {
      out[std::atoi(kv.substr(0, eq).c_str())] = kv.substr(eq + 1);
    }
    if (end == std::string::npos)
    {
      break;
    }
    i = end + 1;
  }
  return out;
}

GatewaySession::Decoder fixDecoder()
{
  return [](const uint8_t* p, size_t n)
  { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); };
}

// What a gateway puts on the wire for a session-level refusal: the reject
// built from the echo, encoded through the session's protocol.
std::unordered_map<int, std::string> wireReject(const RejectEcho& echo, SessionReject reason,
                                                uint64_t account)
{
  const OutboundEvent ev{OrderRejected{echo.id, echo.symbol, toRejectReason(reason), account,
                                       echo.clientOrderId}};
  return fields(FixCodec::encode(ev, /*seq=*/1, "VENUE", "CLIENT", "20260921-00:00:00.000"));
}

}  // namespace

// Every refusal made before the engine sees the frame names the frame.
//
// A reject with id 0 / no ClOrdID is silence dressed as an exec report: the
// client cannot match it to anything it sent, so it waits out its timeout and
// resends -- and the resend comes back refused AGAIN, this time as a duplicate
// ClOrdID. One refusal becomes two and the second one is unexplainable.
TEST(SessionPerimeter, EveryRefusalBeforeTheEngineNamesTheOrder)
{
  constexpr uint64_t kAccount = 7;
  constexpr uint64_t kClOrdId = 424242;
  const std::string frame = newOrderSingle(kClOrdId);
  const auto* p = reinterpret_cast<const uint8_t*>(frame.data());

  // Unauthenticated: the frame was never admitted, but it is still a frame
  // about an order and the refusal says which one.
  {
    flox::RateLimitPolicy none;
    GatewaySession s(kAccount, fixDecoder(), none);
    SessionReject rej{};
    RejectEcho echo{};
    EXPECT_FALSE(s.handle(p, frame.size(), 1000, rej, &echo).has_value());
    ASSERT_EQ(rej, SessionReject::Unauthenticated);
    EXPECT_EQ(echo.clientOrderId, kClOrdId);
    EXPECT_EQ(echo.id, kClOrdId);
    EXPECT_EQ(echo.symbol, kSym);
    auto w = wireReject(echo, rej, kAccount);
    EXPECT_EQ(w[11], std::to_string(kClOrdId));
    EXPECT_EQ(w[37], std::to_string(kClOrdId));
  }

  // RateLimited: the frame decoded, so the identity was in hand all along.
  {
    flox::RateLimitPolicy limits;
    limits.addBucket("t", 1'000'000'000, 1);
    GatewaySession s(kAccount, fixDecoder(), limits);
    s.authenticate(true);
    SessionReject rej{};
    RejectEcho echo{};
    EXPECT_TRUE(s.handle(p, frame.size(), 1000, rej, &echo).has_value());
    EXPECT_FALSE(s.handle(p, frame.size(), 1000, rej, &echo).has_value());
    ASSERT_EQ(rej, SessionReject::RateLimited);
    auto w = wireReject(echo, rej, kAccount);
    EXPECT_EQ(w[11], std::to_string(kClOrdId));
    EXPECT_EQ(w[37], std::to_string(kClOrdId));
  }

  // DecodeError: nothing decoded, so there is no OrderID to give -- but the
  // ClOrdID is legible in the bytes and that is what the client reconciles on.
  {
    flox::RateLimitPolicy none;
    GatewaySession s(kAccount, fixDecoder(), none);
    s.authenticate(true);
    // Side (54) missing: the codec refuses rather than guessing a direction.
    const std::string bad =
        field(35, "D") + field(11, std::to_string(kClOrdId)) + field(55, "11") + field(38, "1");
    SessionReject rej{};
    RejectEcho echo{};
    EXPECT_FALSE(s.handle(reinterpret_cast<const uint8_t*>(bad.data()), bad.size(), 1000, rej,
                          &echo)
                     .has_value());
    ASSERT_EQ(rej, SessionReject::DecodeError);
    EXPECT_EQ(echo.clientOrderId, kClOrdId);
    auto w = wireReject(echo, rej, kAccount);
    EXPECT_EQ(w[11], std::to_string(kClOrdId));
    EXPECT_EQ(w.count(37), 1u);
  }
}

// A frame that does not decode at all -- an unknown MsgType the codec has no
// branch for -- still carries a readable tag 11, and the refusal takes it.
TEST(SessionPerimeter, AnUndecodableFrameIsRefusedWithTheClOrdIdItCarries)
{
  constexpr uint64_t kClOrdId = 99001;
  flox::RateLimitPolicy none;
  GatewaySession s(7, fixDecoder(), none);
  s.authenticate(true);

  const std::string junk = field(35, "ZZ") + field(11, std::to_string(kClOrdId)) + field(55, "11");
  SessionReject rej{};
  RejectEcho echo{};
  EXPECT_FALSE(
      s.handle(reinterpret_cast<const uint8_t*>(junk.data()), junk.size(), 1000, rej, &echo)
          .has_value());
  EXPECT_EQ(rej, SessionReject::DecodeError);
  EXPECT_EQ(echo.clientOrderId, kClOrdId);
  EXPECT_EQ(echo.id, 0u);  // the venue never had an order id for it
}

// The scrape is narrow on purpose: a wrong identifier points the client at an
// order it did not send, which is worse than no identifier at all.
TEST(SessionPerimeter, TheClOrdIdScrapeRefusesWhatItCannotReadAsOne)
{
  const std::string tag411 = field(411, "5") + field(35, "ZZ");
  EXPECT_EQ(clientOrderIdFromRaw(reinterpret_cast<const uint8_t*>(tag411.data()), tag411.size()),
            0u);

  const std::string alpha = field(11, "ORD-7");
  EXPECT_EQ(clientOrderIdFromRaw(reinterpret_cast<const uint8_t*>(alpha.data()), alpha.size()), 0u);

  const std::string empty = field(11, "");
  EXPECT_EQ(clientOrderIdFromRaw(reinterpret_cast<const uint8_t*>(empty.data()), empty.size()), 0u);

  const std::string huge = field(11, "99999999999999999999");
  EXPECT_EQ(clientOrderIdFromRaw(reinterpret_cast<const uint8_t*>(huge.data()), huge.size()), 0u);

  const uint8_t binary[3] = {0xFF, 0x00, 0x11};
  EXPECT_EQ(clientOrderIdFromRaw(binary, sizeof binary), 0u);

  const std::string good = field(35, "D") + field(11, "1234");
  EXPECT_EQ(clientOrderIdFromRaw(reinterpret_cast<const uint8_t*>(good.data()), good.size()), 1234u);
}
