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
#include "flox-venue/session_registry.h"
#include "flox-venue/tcp_gateway.h"
#include "flox/util/transport.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
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
                                                uint64_t account, std::string_view text = {})
{
  const OutboundEvent ev{OrderRejected{echo.id, echo.symbol, toRejectReason(reason), account,
                                       echo.clientOrderId}};
  return fields(FixCodec::encode(ev, /*seq=*/1, "VENUE", "CLIENT", "20260921-00:00:00.000",
                                 /*possDup=*/false, /*origSendingTime=*/{}, text));
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

// ---------------------------------------------------------------------------
// The client rate limit is a setting, and a ban does not travel in silence.
// ---------------------------------------------------------------------------

namespace
{

// Drive `count` order actions through a session at one-millisecond spacing,
// on the policy's own clock. Nothing sleeps: the limiter reads the timestamp
// it is handed, so a test that measures a window must supply the window, not
// wait out a real one.
int admitted(GatewaySession& s, const std::string& frame, int count, int64_t startNs,
             int64_t stepNs)
{
  const auto* p = reinterpret_cast<const uint8_t*>(frame.data());
  int ok = 0;
  for (int i = 0; i < count; ++i)
  {
    SessionReject rej{};
    if (s.handle(p, frame.size(), startNs + static_cast<int64_t>(i) * stepNs, rej).has_value())
    {
      ++ok;
    }
  }
  return ok;
}

}  // namespace

// The profile that used to be hardwired -- 50 actions per 10 seconds, then a
// three-minute ban -- is one exchange's retail tier. For a client bridge that
// fans a price move out into a burst it is an outage, so the policy has to be
// something a deployment chooses, including choosing not to have one.
TEST(SessionPerimeter, TheClientRateLimitIsASettingAndOffIsOneOfItsValues)
{
  const std::string frame = newOrderSingle(1);

  {
    GatewaySession s(7, fixDecoder(), SessionRateLimit::off().policy());
    s.authenticate(true);
    // 200 actions inside one second: every one of them admitted.
    EXPECT_EQ(admitted(s, frame, 200, 1'000'000'000LL, 5'000'000LL), 200);
  }

  {
    SessionRateLimit limit;  // the former hardwired shape, now a default
    EXPECT_EQ(limit.actionsPerWindow, 50u);
    EXPECT_EQ(limit.windowNs, 10'000'000'000LL);
    GatewaySession s(7, fixDecoder(), limit.policy());
    s.authenticate(true);
    constexpr int64_t kStart = 1'000'000'000LL;
    constexpr int64_t kStep = 1'000'000LL;  // 1 ms apart: 51 of them span 50 ms
    EXPECT_EQ(admitted(s, frame, 50, kStart, kStep), 50);

    SessionReject rej{};
    RejectEcho echo{};
    std::string text;
    const int64_t at51 = kStart + 50 * kStep;
    EXPECT_FALSE(s.handle(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), at51, rej,
                          &echo, &text)
                     .has_value());
    EXPECT_EQ(rej, SessionReject::RateLimited);
    // The refusal says what tripped AND how long the window still has to run:
    // the oldest of the 50 charges ages out 10s after it was made, so ~9.95s.
    EXPECT_NE(text.find("RateLimited"), std::string::npos) << text;
    EXPECT_NE(text.find("retry in 9950 ms"), std::string::npos) << text;

    auto w = wireReject(echo, rej, 7, text);
    EXPECT_EQ(w[58], text);
  }
}

// A ban is the venue refusing a client for minutes. Announced twice: to the
// client, with the time it lifts, and to whoever runs the venue, by name.
TEST(SessionPerimeter, ABanNamesItsRemainingTimeToTheClientAndItselfToTheLog)
{
  SessionRateLimit limit;
  limit.actionsPerWindow = 1;
  limit.windowNs = 1'000'000'000LL;
  limit.banAfterRejects = 2;
  limit.banNs = 5'000'000'000LL;

  GatewaySession s(7, fixDecoder(), limit.policy());
  s.setName("tcp/7");
  s.authenticate(true);
  std::vector<SessionBan> bans;
  s.setBanObserver([&bans](const SessionBan& b)
                   { bans.push_back(b); });

  const std::string frame = newOrderSingle(1);
  const auto* p = reinterpret_cast<const uint8_t*>(frame.data());
  constexpr int64_t kStart = 1'000'000'000LL;

  SessionReject rej{};
  RejectEcho echo{};
  std::string text;
  EXPECT_TRUE(s.handle(p, frame.size(), kStart, rej, &echo, &text).has_value());

  // First refusal: the window, not the ban.
  EXPECT_FALSE(s.handle(p, frame.size(), kStart + 1'000'000, rej, &echo, &text).has_value());
  EXPECT_NE(text.find("RateLimited:"), std::string::npos) << text;
  EXPECT_TRUE(bans.empty());

  // Second consecutive refusal arms the ban, and the frame that armed it is
  // the one the client hears about -- everything after is refused too.
  const int64_t banAt = kStart + 2'000'000;
  EXPECT_FALSE(s.handle(p, frame.size(), banAt, rej, &echo, &text).has_value());
  EXPECT_EQ(text, "RateLimitBanned: retry in 5000 ms");
  ASSERT_EQ(bans.size(), 1u);
  EXPECT_EQ(bans[0].session, "tcp/7");
  EXPECT_EQ(bans[0].account, 7u);
  EXPECT_EQ(bans[0].remainingNs, 5'000'000'000LL);

  // Still banned two seconds later: the text counts down rather than repeating
  // itself, and the operational event is not re-announced per refused frame.
  EXPECT_FALSE(s.handle(p, frame.size(), banAt + 2'000'000'000LL, rej, &echo, &text).has_value());
  EXPECT_EQ(text, "RateLimitBanned: retry in 3000 ms");
  EXPECT_EQ(bans.size(), 1u);

  auto w = wireReject(echo, rej, 7, text);
  EXPECT_EQ(w[58], "RateLimitBanned: retry in 3000 ms");
}

// The whole path, on a loopback socket: the refusal the gateway puts on the
// wire carries the wait, not just the word.
TEST(SessionPerimeter, TheWireRefusalCarriesTheWaitThroughTheGateway)
{
  constexpr uint64_t kAccount = 7;
  SessionRegistry registry;
  TcpGateway gw(fixDecoder(), kAccount);

  const std::string sender = "VENUE";
  const std::string target = "CLIENT";
  gw.setDelivery(&registry,
                 [sender, target](const OutboundEvent& e, uint64_t seq, int64_t,
                                  std::vector<uint8_t>& out)
                 {
                   const std::string m =
                       FixCodec::encode(e, seq, sender, target, "20260921-00:00:00.000");
                   out.assign(m.begin(), m.end());
                   return !m.empty();
                 });
  gw.setRejectEncoder([sender, target](const OutboundEvent& e, std::string_view text, uint64_t seq,
                                       int64_t, std::vector<uint8_t>& out)
                      {
                        const std::string m = FixCodec::encode(
                            e, seq, sender, target, "20260921-00:00:00.000", false, {}, text);
                        out.assign(m.begin(), m.end());
                        return !m.empty(); });

  SessionRateLimit limit;
  limit.actionsPerWindow = 1;
  limit.windowNs = 10'000'000'000LL;
  limit.banAfterRejects = 0;  // one refusal at a time, no ban in this test
  gw.setRateLimit(limit);

  const int port = gw.start(0, [](const InboundCommand&, const TcpGateway::Responder&, int64_t) {});
  ASSERT_GT(port, 0);

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(c, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  ASSERT_EQ(::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
  timeval tv{};
  tv.tv_sec = 5;  // deadline: a refusal that never arrives fails, it does not hang
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  for (uint64_t clOrdId : {1001ULL, 1002ULL})
  {
    const std::string m = newOrderSingle(clOrdId, kAccount);
    ASSERT_TRUE(net::writeFrame(c, reinterpret_cast<const uint8_t*>(m.data()), m.size()));
  }

  std::vector<uint8_t> reply;
  ASSERT_TRUE(net::readFrame(c, reply));
  auto w = fields(std::string(reply.begin(), reply.end()));
  EXPECT_EQ(w[35], "8");
  EXPECT_EQ(w[11], "1002");  // the refusal names the order it refused
  EXPECT_NE(w[58].find("RateLimited: retry in "), std::string::npos) << w[58];
  EXPECT_NE(w[58], "RateLimited");  // the bare reason is what this replaced

  ::close(c);
  gw.stop();
}

// ---------------------------------------------------------------------------
// One session, many accounts.
// ---------------------------------------------------------------------------

namespace
{

// A FIX encoder pair for a gateway: the plain one, and the one that carries a
// session refusal's text.
SessionRegistry::Encoder fixEncoder()
{
  return [](const OutboundEvent& e, uint64_t seq, int64_t, std::vector<uint8_t>& out)
  {
    const std::string m = FixCodec::encode(e, seq, "VENUE", "CLIENT", "20260921-00:00:00.000");
    out.assign(m.begin(), m.end());
    return !m.empty();
  };
}

}  // namespace

// A client bridge speaks for many accounts down ONE connection. The gateway
// bound a connection to exactly one, so the bridge got reports for that one
// and nothing for the rest -- every customer but the first was invisible.
TEST(SessionPerimeter, OneSessionSpeaksForItsWholeSetOfAccounts)
{
  constexpr uint64_t kFirst = 7;
  constexpr uint64_t kSecond = 8;
  constexpr uint64_t kStranger = 99;

  SessionRegistry registry;
  TcpGateway gw(fixDecoder(), kFirst);
  gw.setAccounts({kFirst, kSecond});
  gw.setDelivery(&registry, fixEncoder());
  gw.setRateLimit(SessionRateLimit::off());

  // The engine stands in as an acknowledger: every admitted order comes back
  // as an OrderAccepted for the account that actually owns it.
  const int port =
      gw.start(0,
               [&registry](const InboundCommand& c, const TcpGateway::Responder&, int64_t)
               {
                 const auto* o = std::get_if<NewOrder>(&c);
                 if (o == nullptr)
                 {
                   return;
                 }
                 OrderAccepted a;
                 a.id = o->id;
                 a.symbol = o->symbol;
                 a.side = o->side;
                 a.price = o->price;
                 a.leavesQty = o->quantity;
                 a.account = o->accountId;
                 a.clientOrderId = o->clientOrderId;
                 registry.route(OutboundEvent{a});
               });
  ASSERT_GT(port, 0);

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(c, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  ASSERT_EQ(::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
  timeval tv{};
  tv.tv_sec = 5;  // deadline: a report that never arrives fails rather than hangs
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  for (const auto& [clOrdId, account] : std::vector<std::pair<uint64_t, uint64_t>>{
           {101, kFirst}, {102, kSecond}, {103, kStranger}})
  {
    const std::string m = newOrderSingle(clOrdId, account);
    ASSERT_TRUE(net::writeFrame(c, reinterpret_cast<const uint8_t*>(m.data()), m.size()));
  }

  std::unordered_map<std::string, std::string> statusOf;  // ClOrdID -> OrdStatus
  for (int i = 0; i < 3; ++i)
  {
    std::vector<uint8_t> reply;
    ASSERT_TRUE(net::readFrame(c, reply)) << "only " << i << " reports arrived";
    auto w = fields(std::string(reply.begin(), reply.end()));
    statusOf[w[11]] = w[39];
  }

  EXPECT_EQ(statusOf["101"], "0") << "the session's first account was not acknowledged";
  EXPECT_EQ(statusOf["102"], "0") << "the session's second account was not acknowledged";
  EXPECT_EQ(statusOf["103"], "8") << "an order for an account the session does not speak for "
                                     "must be refused, not accepted";

  ::close(c);
  gw.stop();
}

// A trade between two accounts of the SAME session is one event on one socket.
// Keyed per account it would be encoded twice, sequenced twice and delivered
// twice down the one connection that holds both.
TEST(SessionPerimeter, AnEventNamingTwoAccountsOfOneSessionArrivesOnce)
{
  constexpr uint64_t kFirst = 7;
  constexpr uint64_t kSecond = 8;

  SessionRegistry registry;
  std::vector<std::vector<uint8_t>> written;
  auto writer = registry.attach(
      std::vector<uint64_t>{kFirst, kSecond},
      [](const OutboundEvent&, uint64_t seq, int64_t, std::vector<uint8_t>& out)
      {
        out.assign(1, static_cast<uint8_t>(seq));
        return true;
      },
      [&written](const uint8_t* p, size_t n)
      {
        written.emplace_back(p, p + n);
        return true;
      },
      [] {});

  FillHeld held;
  held.heldId = 1;
  held.symbol = kSym;
  held.makerAccount = kFirst;
  held.takerAccount = kSecond;
  registry.route(OutboundEvent{held});

  // One sequence number consumed, not two: the seq space belongs to the
  // session, and a client that saw 1 and 2 for one event could never close the
  // gap it thinks it has.
  EXPECT_EQ(registry.lastSeq(kFirst), 1u);
  EXPECT_EQ(registry.lastSeq(kSecond), 1u) << "the alias must resolve to the same stream";
  EXPECT_EQ(registry.logSlice(kFirst, 1).size(), 1u);

  // ... and an event naming only one of the two still arrives.
  OrderAccepted acc;
  acc.id = 5;
  acc.symbol = kSym;
  acc.account = kSecond;
  registry.route(OutboundEvent{acc});
  EXPECT_EQ(registry.lastSeq(kFirst), 2u);

  writer->stop();
  registry.detach(std::vector<uint64_t>{kFirst, kSecond}, writer);
  EXPECT_EQ(written.size(), 2u);

  // After the detach the borrowed account is free again -- an alias left
  // pointing at a session that is gone would send that account's events to
  // nobody, and hand it the other session's sequence numbers on reconnect.
  EXPECT_EQ(registry.lastSeq(kSecond), 0u);
  EXPECT_EQ(registry.lastSeq(kFirst), 2u);

  uint64_t targets[2] = {0, 0};
  int n = 0;
  SessionRegistry::accountsOf(OutboundEvent{held}, targets, n);
  EXPECT_EQ(n, 2);
  EXPECT_EQ(targets[0], kFirst);
  EXPECT_EQ(targets[1], kSecond);
}
