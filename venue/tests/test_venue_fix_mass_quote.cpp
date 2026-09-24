/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX MassQuote (35=i) / QuoteCancel (35=Z) / QuoteStatusReport (35=AI) --
 * T063: the FIX perimeter gained a decoder for a maker's whole ladder on one
 * symbol, decoding to the SAME QuoteLadder command a non-FIX caller builds
 * directly (see flox-venue/messages.h, flox-venue/engine/quote_mmp.inl,
 * docs/venue/fix-quoting.md).
 *
 * Three kinds of test:
 *  - codec: decode()/encode() in isolation, no engine and no socket -- every
 *    refusal FixCodec::decode makes for MassQuote/QuoteCancel, and the
 *    QuoteNotPermitted -> QuoteStatusReport encode case.
 *  - differential (the literal claim in the task): a MassQuote decoded to a
 *    QuoteLadder and the SAME QuoteLadder built directly must produce the
 *    identical engine event stream, event for event -- the same technique
 *    test_venue_quote_ladder.cpp uses for the ladder-vs-Quotes claim. The
 *    mutation this exists to catch is a decode loop that drops a level.
 *  - session: MassQuote/QuoteCancel/QuoteStatusReport and the quotes-only
 *    admission profile over a real TCP socket, the FixConnection/GatewaySession/
 *    TcpGateway pipeline end to end.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/fix_codec.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/tcp_gateway.h"
#include "flox/util/transport.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t ACCT = 7;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

std::string field(int tag, const std::string& v)
{
  return std::to_string(tag) + "=" + v + std::string(1, FixCodec::SOH);
}

// A raw MassQuote (35=i) BODY (no header/checksum -- FixCodec::decode is
// lenient about an absent CheckSum for exactly this kind of test): `levels`
// two-sided entries stepping away from a 100.00 mid one tick per level, bid
// descending / ask ascending (the order the decoder requires), sizes that
// differ per level so a level read from the wrong slot is visible.
std::string massQuoteMsg(uint64_t account, uint64_t quoteId, SymbolId symbol, int levels,
                         bool invertBid = false, bool invertAsk = false,
                         bool secondSymbolMismatch = false)
{
  std::string b = field(35, "i");
  b += field(1, std::to_string(account));
  b += field(117, std::to_string(quoteId));
  for (int i = 0; i < levels; ++i)
  {
    b += field(299, std::to_string(1000 + i));
    const SymbolId s = (secondSymbolMismatch && i == 1) ? symbol + 1 : symbol;
    b += field(55, std::to_string(s));
    double bidV = invertBid ? (99.99 - 0.01 * (levels - 1 - i)) : (99.99 - 0.01 * i);
    double askV = invertAsk ? (100.01 + 0.01 * (levels - 1 - i)) : (100.01 + 0.01 * i);
    char bid[32];
    char ask[32];
    std::snprintf(bid, sizeof bid, "%.2f", bidV);
    std::snprintf(ask, sizeof ask, "%.2f", askV);
    b += field(132, bid);
    b += field(133, ask);
    b += field(134, std::to_string(1 + i));
    b += field(135, std::to_string(2 + i));
  }
  return b;
}

std::string quoteCancelMsg(uint64_t account, SymbolId symbol)
{
  std::string b = field(35, "Z");
  b += field(1, std::to_string(account));
  b += field(55, std::to_string(symbol));
  b += field(298, "4");  // QuoteCancelType: all
  return b;
}

// The QuoteLadder a MassQuote(ACCT, quoteId, SYM, levels) decodes to, built
// directly -- the reference side of the differential tests below.
QuoteLadder expectedLadder(uint64_t account, uint64_t quoteId, SymbolId symbol, uint8_t levels)
{
  QuoteLadder l;
  l.accountId = account;
  l.clientOrderId = quoteId;
  l.symbol = symbol;
  l.levels = levels;
  l.bidIdBase = FixCodec::quoteLadderIdBase(account, symbol);
  l.askIdBase = l.bidIdBase + kQuoteLadderLevels;
  for (uint8_t i = 0; i < levels; ++i)
  {
    l.level[i].bidPrice = px(99.99 - 0.01 * i);
    l.level[i].bidQty = qty(1.0 + i);
    l.level[i].askPrice = px(100.01 + 0.01 * i);
    l.level[i].askQty = qty(2.0 + i);
  }
  return l;
}

// ---- codec: decode() ----

TEST(FixMassQuote, DecodesToTheDirectlyBuiltQuoteLadder)
{
  const auto cmd = FixCodec::decode(massQuoteMsg(ACCT, 555, SYM, 5));
  ASSERT_TRUE(cmd.has_value());
  const auto* l = std::get_if<QuoteLadder>(&*cmd);
  ASSERT_NE(l, nullptr);

  const QuoteLadder want = expectedLadder(ACCT, 555, SYM, 5);
  EXPECT_EQ(l->accountId, want.accountId);
  EXPECT_EQ(l->clientOrderId, want.clientOrderId);
  EXPECT_EQ(l->symbol, want.symbol);
  EXPECT_EQ(l->levels, want.levels);
  EXPECT_EQ(l->bidIdBase, want.bidIdBase);
  EXPECT_EQ(l->askIdBase, want.askIdBase);
  for (uint8_t i = 0; i < 5; ++i)
  {
    EXPECT_EQ(l->level[i].bidPrice.raw(), want.level[i].bidPrice.raw()) << "level " << (int)i;
    EXPECT_EQ(l->level[i].askPrice.raw(), want.level[i].askPrice.raw()) << "level " << (int)i;
    EXPECT_EQ(l->level[i].bidQty.raw(), want.level[i].bidQty.raw()) << "level " << (int)i;
    EXPECT_EQ(l->level[i].askQty.raw(), want.level[i].askQty.raw()) << "level " << (int)i;
  }
}

TEST(FixMassQuote, MissingAccountRejected)
{
  std::string b = field(35, "i");
  b += field(117, "1");
  b += field(299, "1000");
  b += field(55, std::to_string(SYM));
  b += field(132, "99.99");
  b += field(133, "100.01");
  b += field(134, "1");
  b += field(135, "1");
  EXPECT_FALSE(FixCodec::decode(b).has_value());
}

TEST(FixMassQuote, MissingQuoteIdRejected)
{
  std::string b = field(35, "i");
  b += field(1, std::to_string(ACCT));
  b += field(299, "1000");
  b += field(55, std::to_string(SYM));
  b += field(132, "99.99");
  b += field(133, "100.01");
  b += field(134, "1");
  b += field(135, "1");
  EXPECT_FALSE(FixCodec::decode(b).has_value());
}

TEST(FixMassQuote, MoreThanKQuoteLadderLevelsRejected)
{
  EXPECT_FALSE(FixCodec::decode(massQuoteMsg(ACCT, 1, SYM, kQuoteLadderLevels + 1)).has_value());
}

TEST(FixMassQuote, ExactlyKQuoteLadderLevelsAccepted)
{
  EXPECT_TRUE(FixCodec::decode(massQuoteMsg(ACCT, 1, SYM, kQuoteLadderLevels)).has_value());
}

TEST(FixMassQuote, BidNotDescendingRejected)
{
  EXPECT_FALSE(
      FixCodec::decode(massQuoteMsg(ACCT, 1, SYM, 3, /*invertBid*/ true)).has_value());
}

TEST(FixMassQuote, AskNotAscendingRejected)
{
  EXPECT_FALSE(
      FixCodec::decode(massQuoteMsg(ACCT, 1, SYM, 3, false, /*invertAsk*/ true)).has_value());
}

TEST(FixMassQuote, MixedSymbolRejected)
{
  EXPECT_FALSE(FixCodec::decode(massQuoteMsg(ACCT, 1, SYM, 3, false, false,
                                             /*secondSymbolMismatch*/ true))
                   .has_value());
}

TEST(FixMassQuote, MalformedPriceRejected)
{
  std::string b = field(35, "i");
  b += field(1, std::to_string(ACCT));
  b += field(117, "1");
  b += field(299, "1000");
  b += field(55, std::to_string(SYM));
  b += field(132, "not-a-price");
  b += field(133, "100.01");
  b += field(134, "1");
  b += field(135, "1");
  EXPECT_FALSE(FixCodec::decode(b).has_value());
}

TEST(FixMassQuote, ZeroEntriesRejected)
{
  std::string b = field(35, "i");
  b += field(1, std::to_string(ACCT));
  b += field(117, "1");
  EXPECT_FALSE(FixCodec::decode(b).has_value());
}

// ---- codec: QuoteCancel ----

TEST(FixQuoteCancel, DecodesToZeroLevelLadderOnTheSameIdBlock)
{
  const auto massQuote = FixCodec::decode(massQuoteMsg(ACCT, 42, SYM, 5));
  ASSERT_TRUE(massQuote.has_value());
  const auto* placed = std::get_if<QuoteLadder>(&*massQuote);
  ASSERT_NE(placed, nullptr);

  const auto cancel = FixCodec::decode(quoteCancelMsg(ACCT, SYM));
  ASSERT_TRUE(cancel.has_value());
  const auto* c = std::get_if<QuoteLadder>(&*cancel);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->levels, 0);
  EXPECT_EQ(c->accountId, ACCT);
  EXPECT_EQ(c->symbol, SYM);
  EXPECT_EQ(c->bidIdBase, placed->bidIdBase);
  EXPECT_EQ(c->askIdBase, placed->askIdBase);
}

TEST(FixQuoteCancel, MissingSymbolRejected)
{
  std::string b = field(35, "Z");
  b += field(1, std::to_string(ACCT));
  EXPECT_FALSE(FixCodec::decode(b).has_value());
}

TEST(FixQuoteCancel, MissingAccountRejected)
{
  std::string b = field(35, "Z");
  b += field(55, std::to_string(SYM));
  EXPECT_FALSE(FixCodec::decode(b).has_value());
}

// ---- codec: encode() ----

TEST(FixQuoteStatusEncode, QuoteNotPermittedEncodesAsQuoteStatusReport)
{
  const OutboundEvent ev =
      OrderRejected{1000, SYM, RejectReason::QuoteNotPermitted, ACCT, 99, {}};
  const std::string msg = FixCodec::encode(ev);
  ASSERT_FALSE(msg.empty());
  auto f = FixCodec::parseFields(msg);
  EXPECT_EQ(f[35], "AI");
  EXPECT_EQ(f[297], "5");
  EXPECT_EQ(f[117], "99");
  EXPECT_EQ(f[55], std::to_string(SYM));
  EXPECT_EQ(f.count(58), 1U);
}

TEST(FixQuoteStatusEncode, OtherOrderRejectedStaysAnExecutionReport)
{
  const OutboundEvent ev =
      OrderRejected{1000, SYM, RejectReason::RestingNotPermitted, ACCT, 99, {}};
  const std::string msg = FixCodec::encode(ev);
  ASSERT_FALSE(msg.empty());
  auto f = FixCodec::parseFields(msg);
  EXPECT_EQ(f[35], "8");
  EXPECT_EQ(f[39], "8");
}

// ---- differential: MassQuote-decoded ladder vs. the same ladder built
// directly. Mirrors test_venue_quote_ladder.cpp's Venue/expectSameStream. ----

struct Venue
{
  std::vector<OutboundEvent> events;
  uint64_t stream{1469598103934665603ULL};
  MatchingEngine<MatchingBook> eng;
  int64_t ts{1};

  Venue()
      : eng(cfg(), [this](const OutboundEvent& e)
            {
              events.push_back(e);
              stream = hashEvent(stream, e); })
  {
  }
  Venue(const Venue&) = delete;
  Venue& operator=(const Venue&) = delete;

  void push(const InboundCommand& c) { eng.submit(c, ts); }
};

void expectSameStream(const Venue& a, const Venue& b)
{
  ASSERT_EQ(a.events.size(), b.events.size()) << "different number of events";
  for (size_t i = 0; i < a.events.size(); ++i)
  {
    uint64_t ha = 1469598103934665603ULL;
    uint64_t hb = ha;
    EXPECT_EQ(hashEvent(ha, a.events[i]), hashEvent(hb, b.events[i])) << "event " << i << " differs";
  }
  EXPECT_EQ(a.stream, b.stream);
  EXPECT_EQ(a.eng.stateHash(), b.eng.stateHash());
  EXPECT_EQ(a.eng.restingOrderCount(), b.eng.restingOrderCount());
}

TEST(FixMassQuote, EventStreamMatchesTheDirectlyBuiltQuoteLadder)
{
  const auto cmd = FixCodec::decode(massQuoteMsg(ACCT, 555, SYM, 5));
  ASSERT_TRUE(cmd.has_value());

  Venue viaFix;
  viaFix.push(*cmd);

  Venue viaDirect;
  viaDirect.push(InboundCommand{expectedLadder(ACCT, 555, SYM, 5)});

  EXPECT_EQ(viaFix.eng.restingOrderCount(), 10U);
  expectSameStream(viaFix, viaDirect);
}

TEST(FixQuoteCancel, EventStreamMatchesTheDirectlyBuiltZeroLevelQuoteLadder)
{
  Venue viaFix;
  viaFix.push(*FixCodec::decode(massQuoteMsg(ACCT, 555, SYM, 5)));
  const auto cancel = FixCodec::decode(quoteCancelMsg(ACCT, SYM));
  ASSERT_TRUE(cancel.has_value());
  viaFix.push(*cancel);

  Venue viaDirect;
  viaDirect.push(InboundCommand{expectedLadder(ACCT, 555, SYM, 5)});
  QuoteLadder takeDown = expectedLadder(ACCT, 555, SYM, 5);
  takeDown.levels = 0;
  takeDown.clientOrderId = 0;  // a real QuoteCancel names no clOrdId of its own
  viaDirect.push(InboundCommand{takeDown});

  EXPECT_EQ(viaFix.eng.restingOrderCount(), 0U);
  expectSameStream(viaFix, viaDirect);
}

// ---- session: over a real TCP socket ----

using Fields = std::unordered_map<int, std::string>;

struct FixVenue
{
  GatewayCounters counters;
  SessionRegistry registry;
  std::mutex m;
  MatchingEngine<MatchingBook> eng;
  FixSessionHost fixHost;

  explicit FixVenue()
      : registry(DeliveryConfig{}, &counters),
        eng(cfg(), [this](const OutboundEvent& e)
            { registry.route(e); }),
        fixHost(fixCfg())
  {
  }

  static FixSessionConfig fixCfg()
  {
    FixSessionConfig c;
    c.senderCompId = "VENUE";
    c.targetCompId = "CLIENT";
    return c;
  }

  static SessionRegistry::Encoder encoder(const FixSessionConfig& fc)
  {
    return [fc](const OutboundEvent& e, uint64_t seq, int64_t tsNs, std::vector<uint8_t>& out)
    {
      const std::string msg =
          FixCodec::encode(e, seq, fc.senderCompId, fc.targetCompId, FixSession::sendingTime(tsNs));
      if (msg.empty())
      {
        return false;
      }
      out.assign(msg.begin(), msg.end());
      return true;
    };
  }

  std::unique_ptr<TcpGateway> gateway(uint64_t account)
  {
    auto gw = std::make_unique<TcpGateway>(
        [](const uint8_t* p, size_t n)
        { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); },
        account);
    gw->setDelivery(&registry, encoder(fixHost.config()));
    gw->setFixSession(&fixHost);
    gw->setCounters(&counters);
    return gw;
  }

  TcpGateway::Handler handler()
  {
    return [this](const InboundCommand& c, const TcpGateway::Responder&, int64_t)
    { submit(c); };
  }

  void submit(const InboundCommand& c)
  {
    std::lock_guard<std::mutex> lk(m);
    eng.submit(c);
  }

  void setQuotesOnly(uint64_t account)
  {
    std::lock_guard<std::mutex> lk(m);
    AdmissionProfile p;
    p.deny = AdmissionDeny::DenyNewOrder | AdmissionDeny::DenyCancel;
    eng.setAdmissionProfile(account, p);
  }

  size_t openOrders(uint64_t account)
  {
    std::lock_guard<std::mutex> lk(m);
    return eng.snapshotAccount(account).openOrders.size();
  }
};

// wallClockNs() comes from flox-venue/session_registry.h -- the same clock
// the gateway/registry stamp SendingTime with.

// Scripted FIX counterparty, the same shape as test_venue_fix_session.cpp's
// FixClient (kept local: each test file in this suite owns its own copy).
struct FixClient
{
  int fd{-1};
  uint64_t seq{1};

  bool connectTo(int port, int rcvTimeoutSec = 4)
  {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
    {
      return false;
    }
    timeval tv{rcvTimeoutSec, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return true;
  }

  void send(const std::string& m) { net::writeFrame(fd, reinterpret_cast<const uint8_t*>(m.data()), m.size()); }

  void admin(const std::string& type, const std::vector<std::pair<int, std::string>>& fields = {})
  {
    send(FixCodec::encodeAdmin(type, seq++, "CLIENT", "VENUE", FixSession::sendingTime(wallClockNs()),
                               fields));
  }

  // Send a MassQuote/QuoteCancel BODY (see massQuoteMsg/quoteCancelMsg above)
  // wrapped with the full session header this transport requires (34/49/56/
  // 52). Deliberately NOT built via FixCodec::parseFields: that collapses
  // repeats into a map (last occurrence wins), which is exactly wrong for a
  // MassQuote's repeating QuoteEntry groups -- the body's own field bytes
  // (order and repeats both) are kept verbatim, only the leading "35=..."
  // field is replaced with a properly header-sequenced one.
  void appMessage(const std::string& body)
  {
    const size_t firstSoh = body.find(FixCodec::SOH);
    const std::string type = body.substr(3, firstSoh - 3);  // after "35="
    const std::string rest = body.substr(firstSoh + 1);
    std::string b;
    auto add = [&](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + std::string(1, FixCodec::SOH); };
    add(35, type);
    add(34, std::to_string(seq++));
    add(49, "CLIENT");
    add(56, "VENUE");
    add(52, FixSession::sendingTime(wallClockNs()));
    b += rest;
    send(FixCodec::frame(b));
  }

  void order(uint64_t id, const char* side, const char* price, const char* quantity)
  {
    std::string b;
    auto add = [&](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + std::string(1, FixCodec::SOH); };
    add(35, "D");
    add(34, std::to_string(seq++));
    add(49, "CLIENT");
    add(56, "VENUE");
    add(52, FixSession::sendingTime(wallClockNs()));
    add(11, std::to_string(id));
    add(55, std::to_string(SYM));
    add(54, side);
    add(38, quantity);
    add(44, price);
    add(40, "2");
    send(FixCodec::frame(b));
  }

  void cancel(uint64_t id)
  {
    std::string b;
    auto add = [&](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + std::string(1, FixCodec::SOH); };
    add(35, "F");
    add(34, std::to_string(seq++));
    add(49, "CLIENT");
    add(56, "VENUE");
    add(52, FixSession::sendingTime(wallClockNs()));
    add(41, std::to_string(id));
    add(55, std::to_string(SYM));
    send(FixCodec::frame(b));
  }

  bool read(Fields& f)
  {
    std::vector<uint8_t> frame;
    if (!net::readFrame(fd, frame))
    {
      return false;
    }
    f = FixCodec::parseFields(std::string(frame.begin(), frame.end()));
    return true;
  }

  bool readType(const std::string& type, Fields& f)
  {
    while (read(f))
    {
      if (f.count(35) != 0 && f[35] == type)
      {
        return true;
      }
    }
    return false;
  }

  void close()
  {
    if (fd >= 0)
    {
      ::close(fd);
      fd = -1;
    }
  }
  ~FixClient() { close(); }
};

uint64_t u64f(Fields& f, int tag)
{
  return f.count(tag) != 0 ? std::strtoull(f[tag].c_str(), nullptr, 10) : 0;
}

TEST(FixMassQuoteSession, MassQuoteIsAckedThenPlacesTheLadder)
{
  FixVenue v;
  auto gw = v.gateway(ACCT);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  FixClient c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  c.appMessage(massQuoteMsg(ACCT, 777, SYM, 5));
  ASSERT_TRUE(c.readType("AI", f));
  EXPECT_EQ(f[297], "0");
  EXPECT_EQ(f[117], "777");
  EXPECT_EQ(f[55], std::to_string(SYM));

  // Ten legs (5 bid, 5 ask), each its own accept -- the same event stream a
  // direct QuoteLadder submission produces.
  int accepts = 0;
  for (int i = 0; i < 10; ++i)
  {
    ASSERT_TRUE(c.readType("8", f));
    if (f[150] == "0")
    {
      ++accepts;
    }
  }
  EXPECT_EQ(accepts, 10);
  EXPECT_EQ(v.openOrders(ACCT), 10U);

  c.close();
  gw->stop();
}

TEST(FixMassQuoteSession, QuoteCancelAckedThenTakesTheLadderDown)
{
  FixVenue v;
  auto gw = v.gateway(ACCT);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  FixClient c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  c.appMessage(massQuoteMsg(ACCT, 1, SYM, 5));
  ASSERT_TRUE(c.readType("AI", f));
  for (int i = 0; i < 10; ++i)
  {
    ASSERT_TRUE(c.readType("8", f));
  }
  ASSERT_EQ(v.openOrders(ACCT), 10U);

  c.appMessage(quoteCancelMsg(ACCT, SYM));
  ASSERT_TRUE(c.readType("AI", f));
  EXPECT_EQ(f[297], "0");

  int cancels = 0;
  for (int i = 0; i < 10; ++i)
  {
    ASSERT_TRUE(c.readType("8", f));
    if (f[150] == "4")
    {
      ++cancels;
    }
  }
  EXPECT_EQ(cancels, 10);
  EXPECT_EQ(v.openOrders(ACCT), 0U);

  c.close();
  gw->stop();
}

TEST(FixMassQuoteSession, TooManyLevelsRejectedWithReasonAndNeverReachesTheEngine)
{
  FixVenue v;
  auto gw = v.gateway(ACCT);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  FixClient c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  c.appMessage(massQuoteMsg(ACCT, 2, SYM, kQuoteLadderLevels + 1));
  ASSERT_TRUE(c.readType("AI", f));
  EXPECT_EQ(f[297], "5");
  EXPECT_EQ(f.count(58), 1U);
  EXPECT_EQ(v.openOrders(ACCT), 0U);

  c.close();
  gw->stop();
}

TEST(FixMassQuoteSession, QuotesOnlyProfileRefusesNewOrderButAllowsMassQuote)
{
  FixVenue v;
  v.setQuotesOnly(ACCT);
  auto gw = v.gateway(ACCT);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  FixClient c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  c.order(1, "1", "100", "5");
  ASSERT_TRUE(c.readType("8", f));
  EXPECT_EQ(f[39], "8");  // Rejected
  EXPECT_EQ(f[58], "NewOrderNotPermitted");
  EXPECT_EQ(v.openOrders(ACCT), 0U);

  c.appMessage(massQuoteMsg(ACCT, 3, SYM, 2));
  ASSERT_TRUE(c.readType("AI", f));
  EXPECT_EQ(f[297], "0");
  for (int i = 0; i < 4; ++i)
  {
    ASSERT_TRUE(c.readType("8", f));
  }
  EXPECT_EQ(v.openOrders(ACCT), 4U);

  c.close();
  gw->stop();
}

TEST(FixMassQuoteSession, QuotesOnlyProfileRefusesCancel)
{
  FixVenue v;
  auto gw = v.gateway(ACCT);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  FixClient c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  // Rest a plain order first (the profile is applied AFTER, so this order
  // itself is unaffected), then deny NewOrder/Cancel and try to cancel it.
  c.order(1, "1", "90", "5");
  ASSERT_TRUE(c.readType("8", f));
  ASSERT_EQ(v.openOrders(ACCT), 1U);

  v.setQuotesOnly(ACCT);
  c.cancel(1);
  // A refused cancel answers OrderCancelReject (35=9), not an execution
  // report -- FIX 4.4 answers a refused 35=F with a different message
  // category (see FixCodec::encode's CancelRejected case).
  ASSERT_TRUE(c.readType("9", f));
  EXPECT_EQ(f[58], "CancelNotPermitted");
  EXPECT_EQ(v.openOrders(ACCT), 1U);

  c.close();
  gw->stop();
}

// The refusal the maker actually reads. FixCodec::decode names the tag it
// refused; QuoteStatusReport's Text (58) is the only place that name reaches
// the sender, and a session that answers "malformed" leaves a maker to guess
// which of its tags is wrong from a message it cannot see the venue parse.
// Each case below is refused by the codec for a different field, so the text
// has to change with it -- a constant string passes none of them.
TEST(FixMassQuoteSession, QuoteStatusReportTextCarriesTheCodecsReason)
{
  FixVenue v;
  auto gw = v.gateway(ACCT);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  FixClient c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  struct Case
  {
    const char* what;
    std::string body;
    const char* names;
  };

  // QuoteCancel with an Account FIX types as String and this venue carries
  // as an integer; the same with an Account above the range a quoting id
  // block is derived for (docs/venue/fix-quoting.md, "The range"); a
  // MassQuote whose entry names a Symbol that is not one; and a MassQuote
  // whose ladder does not step away from the mid.
  std::string badAccount = field(35, "Z") + field(1, "ACME") + field(55, std::to_string(SYM));
  std::string outOfRange =
      field(35, "Z") + field(1, "16777216") + field(55, std::to_string(SYM));
  std::string badSymbol = field(35, "i") + field(1, std::to_string(ACCT)) + field(117, "9001") +
                          field(299, "1000") + field(55, "BTC-USD") + field(132, "99.99") +
                          field(133, "100.01") + field(134, "1") + field(135, "2");
  std::string badBidPx = field(35, "i") + field(1, std::to_string(ACCT)) + field(117, "9002") +
                         field(299, "1000") + field(55, std::to_string(SYM)) +
                         field(132, "not-a-price") + field(133, "100.01") + field(134, "1") +
                         field(135, "2");

  const Case cases[] = {
      {"35=Z Account(1) is not an integer", badAccount, "Account(1)"},
      {"35=Z Account(1) above the quoting range", outOfRange, "Account(1)"},
      {"35=i Symbol(55) is not an integer", badSymbol, "Symbol(55)"},
      {"35=i BidPx(132) is not a price", badBidPx, "BidPx(132)"},
  };

  for (const Case& t : cases)
  {
    c.appMessage(t.body);
    ASSERT_TRUE(c.readType("AI", f)) << t.what;
    EXPECT_EQ(f[297], "5") << t.what;
    ASSERT_EQ(f.count(58), 1U) << t.what;
    EXPECT_NE(f[58].find(t.names), std::string::npos)
        << t.what << ": Text (58) was \"" << f[58] << "\", which does not name " << t.names;
    EXPECT_EQ(f[58].find("malformed"), std::string::npos)
        << t.what << ": Text (58) fell back to the generic refusal: \"" << f[58] << "\"";
  }

  EXPECT_EQ(v.openOrders(ACCT), 0U);

  c.close();
  gw->stop();
}

}  // namespace
