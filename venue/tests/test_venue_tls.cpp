/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sbe.h"
#include "flox-venue/sbe_order_entry_codec.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/tls_gateway.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
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
NewOrder mk(OrderId id, Side s, double p, double q, uint64_t acct = 1)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}
void test_tls_roundtrip()
{
  std::printf("test_tls\n");
  std::mutex m;
  TlsGateway::Responder currentResp;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     std::vector<uint8_t> b;
                                     SbeOrderEntryCodec::encode(e, b);
                                     if (!b.empty() && currentResp){ currentResp(b.data(), b.size());
} });

  TlsGateway gw([](const uint8_t* p, size_t n)
                { return SbeOrderEntryCodec::decode(p, n); });
  const int port = gw.start(0, [&](const InboundCommand& c, const TlsGateway::Responder& r)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              currentResp = r;
                              eng.submit(c);
                              currentResp = {}; });
  CHECK(port > 0);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);

  SSL_CTX* cctx = tls::clientCtx();
  SSL* ssl = SSL_new(cctx);
  SSL_set_fd(ssl, fd);
  CHECK(SSL_connect(ssl) == 1);
  CHECK(std::string(SSL_get_version(ssl)).rfind("TLS", 0) == 0);  // negotiated TLS

  timeval tv{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  auto sendCmd = [&](const InboundCommand& c)
  {
    std::vector<uint8_t> b;
    SbeOrderEntryCodec::encode(c, b);
    tls::writeFrame(ssl, b.data(), b.size());
  };
  sendCmd(InboundCommand{mk(1, Side::SELL, 100, 5, 1)});
  sendCmd(InboundCommand{mk(2, Side::BUY, 100, 3, 2)});

  std::set<uint8_t> tags;
  std::vector<uint8_t> frame;
  while (tls::readFrame(ssl, frame))
  {
    if (!frame.empty())
    {
      tags.insert(static_cast<uint8_t>(SbeOrderEntryCodec::templateId(frame.data(), frame.size())));
    }
    if (tags.count(uint8_t(SbeOrderEntryCodec::OutTmpl::Accepted)) && tags.count(uint8_t(SbeOrderEntryCodec::OutTmpl::Trade)) &&
        tags.count(uint8_t(SbeOrderEntryCodec::OutTmpl::Executed)))
    {
      break;
    }
  }
  SSL_shutdown(ssl);
  SSL_free(ssl);
  ::close(fd);
  SSL_CTX_free(cctx);
  gw.stop();

  CHECK(tags.count(uint8_t(SbeOrderEntryCodec::OutTmpl::Accepted)) == 1);
  CHECK(tags.count(uint8_t(SbeOrderEntryCodec::OutTmpl::Trade)) == 1);
  CHECK(tags.count(uint8_t(SbeOrderEntryCodec::OutTmpl::Executed)) == 1);
}

// A market maker quoting over the encrypted path must get the same disconnect
// safety net as the plaintext path: on drop, its resting quote is pulled.
void test_tls_cancel_on_disconnect()
{
  std::printf("test_tls_cancel_on_disconnect\n");
  std::mutex m;
  TlsGateway::Responder currentResp;
  std::vector<OutboundEvent> events;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     events.push_back(e);
                                     std::vector<uint8_t> b;
                                     SbeOrderEntryCodec::encode(e, b);
                                     if (!b.empty() && currentResp){ currentResp(b.data(), b.size());
} });
  TlsGateway gw([](const uint8_t* p, size_t n)
                { return SbeOrderEntryCodec::decode(p, n); });
  gw.setCancelOnDisconnect(true);
  const int port = gw.start(0, [&](const InboundCommand& c, const TlsGateway::Responder& r)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              currentResp = r;
                              eng.submit(c);
                              currentResp = {}; });
  CHECK(port > 0);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  SSL_CTX* cctx = tls::clientCtx();
  SSL* ssl = SSL_new(cctx);
  SSL_set_fd(ssl, fd);
  CHECK(SSL_connect(ssl) == 1);
  timeval tv{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  std::vector<uint8_t> b;
  SbeOrderEntryCodec::encode(InboundCommand{mk(1, Side::SELL, 100, 5, 1)}, b);
  tls::writeFrame(ssl, b.data(), b.size());
  std::vector<uint8_t> frame;
  tls::readFrame(ssl, frame);  // wait for Accepted -> resting

  SSL_shutdown(ssl);
  SSL_free(ssl);
  ::close(fd);
  SSL_CTX_free(cctx);
  gw.stop();  // disconnect -> cancel-on-disconnect fires over TLS

  CHECK(eng.book().empty());
  bool canceled = false;
  for (const auto& e : events)
  {
    if (const auto* x = std::get_if<OrderCanceled>(&e); x && x->id == 1)
    {
      canceled = true;
    }
  }
  CHECK(canceled);
}

// TLS client with the same read/until helpers the plaintext delivery test
// uses, but over an SSL* (the venue-side framing is identical).
struct TlsClient
{
  int fd{-1};
  SSL_CTX* ctx{nullptr};
  SSL* ssl{nullptr};

  bool connectTo(int port)
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
    ctx = tls::clientCtx();
    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1)
    {
      return false;
    }
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return true;
  }
  void sendCmd(const InboundCommand& c)
  {
    std::vector<uint8_t> b;
    SbeOrderEntryCodec::encode(c, b);
    tls::writeFrame(ssl, b.data(), b.size());
  }
  // Read until a frame with `wantTmpl` arrives (or timeout); returns it.
  bool readUntil(uint8_t wantTmpl, std::vector<uint8_t>& out)
  {
    std::vector<uint8_t> f;
    while (tls::readFrame(ssl, f))
    {
      if (!f.empty() &&
          static_cast<uint8_t>(SbeOrderEntryCodec::templateId(f.data(), f.size())) == wantTmpl)
      {
        out = f;
        return true;
      }
    }
    return false;
  }
  void close()
  {
    if (ssl != nullptr)
    {
      SSL_shutdown(ssl);
      SSL_free(ssl);
      ssl = nullptr;
    }
    if (fd >= 0)
    {
      ::close(fd);
      fd = -1;
    }
    if (ctx != nullptr)
    {
      SSL_CTX_free(ctx);
      ctx = nullptr;
    }
  }
  ~TlsClient() { close(); }
};

// Delivery through the SessionRegistry over TLS: the asynchronous maker exec
// report reaches the MAKER's TLS session, not the aggressor's -- the TLS
// analog of test_venue_delivery's cross-session case. The per-connection SSL
// mutex is what makes the writer-thread SSL_write legal next to the reader's
// SSL_read polls.
void test_tls_cross_session_delivery()
{
  std::printf("test_tls_cross_session_delivery\n");
  GatewayCounters counters;
  SessionRegistry registry({}, &counters);
  std::mutex m;  // serializes engine access across gateway handler threads
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { registry.route(e); });
  const SessionRegistry::Encoder encoder =
      [](const OutboundEvent& e, uint64_t seq, int64_t, std::vector<uint8_t>& out)
  {
    SbeOrderEntryCodec::encode(e, out, seq);
    return !out.empty();
  };
  auto gateway = [&](uint64_t account)
  {
    auto gw = std::make_unique<TlsGateway>(
        [](const uint8_t* p, size_t n)
        { return SbeOrderEntryCodec::decode(p, n); },
        account);
    gw->setDelivery(&registry, encoder);
    gw->setCounters(&counters);
    return gw;
  };
  const TlsGateway::Handler handler = [&](const InboundCommand& c, const TlsGateway::Responder&)
  {
    std::lock_guard<std::mutex> lk(m);
    eng.submit(c);
  };

  auto gwA = gateway(1);
  auto gwB = gateway(2);
  const int pA = gwA->start(0, handler);
  const int pB = gwB->start(0, handler);
  CHECK(pA > 0 && pB > 0);

  TlsClient a;
  TlsClient b;
  CHECK(a.connectTo(pA) && b.connectTo(pB));

  // Maker rests on session A.
  a.sendCmd(InboundCommand{mk(1, Side::SELL, 100, 5)});
  std::vector<uint8_t> f;
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
  CHECK(sbe::getU64(f.data() + sbe::kHeaderSize) == 1);  // orderId
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) == 1);

  // Aggressor on session B fills it.
  b.sendCmd(InboundCommand{mk(2, Side::BUY, 100, 5, 2)});
  CHECK(b.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Executed), f));
  CHECK(sbe::getU64(f.data() + sbe::kHeaderSize) == 2);  // the taker's own report

  // The maker fill arrives on session A -- an event A never asked for on this
  // frame exchange (the per-frame responder had no path here).
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Executed), f));
  CHECK(sbe::getU64(f.data() + sbe::kHeaderSize) == 1);  // maker order id
  CHECK(f[sbe::kHeaderSize + 37] == 1);                  // complete flag (5 of 5 filled)
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) > 1);

  a.close();
  b.close();
  gwA->stop();
  gwB->stop();
}

// FIX session layer over TLS (setFixSession): Logon 141=Y, a trading cycle,
// the venue's own Heartbeat on the poll tick, and a short resend (PossDup
// replay with the original seq). Same wire shape as TCP -- one FIX message
// per length-prefixed frame -- inside the TLS stream; the full recovery
// matrix stays on the TCP tests.
void test_tls_fix_session()
{
  std::printf("test_tls_fix_session\n");
  GatewayCounters counters;
  SessionRegistry registry({}, &counters);
  std::mutex m;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { registry.route(e); });
  FixSessionHost fixHost;
  const SessionRegistry::Encoder encoder =
      [](const OutboundEvent& e, uint64_t seq, int64_t tsNs, std::vector<uint8_t>& out)
  {
    const std::string msg =
        FixCodec::encode(e, seq, "VENUE", "CLIENT", FixSession::sendingTime(tsNs));
    if (msg.empty())
    {
      return false;
    }
    out.assign(msg.begin(), msg.end());
    return true;
  };
  TlsGateway gw([](const uint8_t* p, size_t n)
                { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); },
                /*account*/ 1);
  gw.setDelivery(&registry, encoder);
  gw.setFixSession(&fixHost);
  gw.setCounters(&counters);
  const int port = gw.start(0, [&](const InboundCommand& c, const TlsGateway::Responder&)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              eng.submit(c); });
  CHECK(port > 0);

  TlsClient c;
  CHECK(c.connectTo(port));
  uint64_t seq = 1;
  const auto sendFix = [&](const std::string& msg)
  { tls::writeFrame(c.ssl, reinterpret_cast<const uint8_t*>(msg.data()), msg.size()); };
  const auto admin = [&](const std::string& type,
                         const std::vector<std::pair<int, std::string>>& fields = {})
  {
    sendFix(FixCodec::encodeAdmin(type, seq++, "CLIENT", "VENUE",
                                  FixSession::sendingTime(wallClockNs()), fields));
  };
  using Fields = std::unordered_map<int, std::string>;
  const auto readType = [&](const std::string& type, Fields& f)
  {
    std::vector<uint8_t> frame;
    while (tls::readFrame(c.ssl, frame))
    {
      f = FixCodec::parseFields(std::string(frame.begin(), frame.end()));
      if (f.count(35) != 0 && f[35] == type)
      {
        return true;
      }
    }
    return false;
  };
  const auto u64f = [](Fields& f, int tag)
  { return f.count(tag) != 0 ? std::strtoull(f[tag].c_str(), nullptr, 10) : 0ULL; };

  admin("A", {{108, "1"}, {141, "Y"}});
  Fields f;
  CHECK(readType("A", f));
  CHECK(u64f(f, 34) == 1 && f[141] == "Y" && u64f(f, 108) == 1);

  {
    std::string b;
    auto add = [&](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + std::string(1, FixCodec::SOH); };
    add(35, "D");
    add(34, std::to_string(seq++));
    add(49, "CLIENT");
    add(56, "VENUE");
    add(52, FixSession::sendingTime(wallClockNs()));
    add(11, "1");
    add(55, "1");
    add(54, "2");
    add(38, "5");
    add(44, "100");
    add(40, "2");
    sendFix(FixCodec::frame(b));
  }
  CHECK(readType("8", f));
  CHECK(u64f(f, 34) == 2 && f[150] == "0" && u64f(f, 37) == 1);

  // Idle past HeartBtInt: the venue heartbeats on the poll tick.
  CHECK(readType("0", f));
  CHECK(u64f(f, 34) >= 3);
  admin("0");

  // Short resend: the Accepted (seq 2) replays with PossDup + OrigSendingTime
  // at its ORIGINAL seq; the shared resend counter is bumped.
  admin("2", {{7, "2"}, {16, "2"}});
  CHECK(readType("8", f));
  CHECK(u64f(f, 34) == 2 && f[43] == "Y" && f.count(122) != 0 && u64f(f, 37) == 1);
  CHECK(counters.resendServed.load() >= 1);

  admin("5");
  CHECK(readType("5", f));  // Logout confirmed

  c.close();
  gw.stop();
}

}  // namespace

TEST(Tls, EngineSuite)
{
  test_tls_roundtrip();
  test_tls_cancel_on_disconnect();
  test_tls_cross_session_delivery();
  test_tls_fix_session();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
