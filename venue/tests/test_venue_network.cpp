/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/control_server.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/ouch_codec.h"
#include "flox-venue/rest_json.h"
#include "flox-venue/session.h"
#include "flox-venue/tcp_gateway.h"
#include "flox-venue/udp_multicast.h"
#include "flox-venue/ws_gateway.h"
#include "flox/book/matching_book.h"
#include "flox/util/crypto.h"
#include "flox/util/websocket.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <chrono>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

void test_session()
{
  std::printf("test_session\n");
  flox::RateLimitPolicy limits;
  limits.addBucket("t", 1'000'000'000, 3);  // 3 actions / 1s window
  GatewaySession s(7, [](const uint8_t* p, size_t n)
                   { return OuchCodec::decode(p, n); }, limits);

  std::vector<uint8_t> buf;
  OuchCodec::encode(InboundCommand{mk(1, Side::BUY, 100, 1)}, buf);

  SessionReject rej{};
  CHECK(!s.handle(buf.data(), buf.size(), 1000, rej).has_value());
  CHECK(rej == SessionReject::Unauthenticated);

  s.authenticate(true);
  CHECK(s.handle(buf.data(), buf.size(), 1000, rej).has_value());
  CHECK(s.handle(buf.data(), buf.size(), 1000, rej).has_value());
  CHECK(s.handle(buf.data(), buf.size(), 1000, rej).has_value());
  CHECK(!s.handle(buf.data(), buf.size(), 1000, rej).has_value());  // 4th within window
  CHECK(rej == SessionReject::RateLimited);

  uint8_t junk[3] = {0xFF, 0x00, 0x11};
  CHECK(!s.handle(junk, sizeof junk, 2'000'000'000, rej).has_value());  // next window
  CHECK(rej == SessionReject::DecodeError);
}

void test_tcp_gateway()
{
  std::printf("test_tcp_gateway\n");
  std::mutex m;
  TcpGateway::Responder currentResp;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     std::vector<uint8_t> b;
                                     OuchCodec::encode(e, b);
                                     if (!b.empty() && currentResp){ currentResp(b.data(), b.size());
} });

  TcpGateway gw([](const uint8_t* p, size_t n)
                { return OuchCodec::decode(p, n); });
  const int port = gw.start(0, [&](const InboundCommand& c, const TcpGateway::Responder& r)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              currentResp = r;
                              eng.submit(c);
                              currentResp = {}; });
  CHECK(port > 0);
  if (port <= 0)
  {
    return;
  }

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  CHECK(::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);

  auto sendCmd = [&](const InboundCommand& cmd)
  {
    std::vector<uint8_t> b;
    OuchCodec::encode(cmd, b);
    net::writeFrame(c, b.data(), b.size());
  };
  sendCmd(InboundCommand{mk(1, Side::SELL, 100, 5, 1)});
  sendCmd(InboundCommand{mk(2, Side::BUY, 100, 3, 2)});

  timeval tv{1, 0};
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::set<uint8_t> tags;
  std::vector<uint8_t> frame;
  while (net::readFrame(c, frame))
  {
    if (!frame.empty())
    {
      tags.insert(frame[0]);
    }
    if (tags.count(static_cast<uint8_t>(OuchOut::Accepted)) &&
        tags.count(static_cast<uint8_t>(OuchOut::Trade)) &&
        tags.count(static_cast<uint8_t>(OuchOut::Executed)))
    {
      break;
    }
  }
  ::close(c);
  gw.stop();

  CHECK(tags.count(static_cast<uint8_t>(OuchOut::Accepted)) == 1);
  CHECK(tags.count(static_cast<uint8_t>(OuchOut::Trade)) == 1);
  CHECK(tags.count(static_cast<uint8_t>(OuchOut::Executed)) == 1);
}

int deliverOverUdp(bool multicast)
{
  const char* group = "239.7.7.7";
  UdpMdSubscriber sub;
  if (!sub.join(group, 0, multicast))
  {
    return -1;
  }
  sub.setTimeout(300);
  UdpMdPublisher pub;
  if (!pub.open(multicast ? group : "127.0.0.1", static_cast<uint16_t>(sub.port()), multicast))
  {
    return -1;
  }

  constexpr int K = 5;
  for (int i = 1; i <= K; ++i)
  {
    pub.publish(MdMessage{MdType::Trade, static_cast<uint64_t>(i), SYM, static_cast<uint64_t>(i),
                          Side::BUY, px(100 + i), qty(i), 0});
  }

  int got = 0;
  MdMessage m;
  while (got < K && sub.recv(m))
  {
    const int i = static_cast<int>(m.id);
    if (m.type == MdType::Trade && m.price == px(100 + i) && m.qty == qty(i))
    {
      ++got;
    }
  }
  return got;
}

void test_udp_md()
{
  std::printf("test_udp_md\n");
  int got = deliverOverUdp(/*multicast*/ true);
  const char* path = "multicast";
  if (got <= 0)
  {
    got = deliverOverUdp(/*multicast*/ false);  // fallback where multicast is blocked
    path = "unicast";
  }
  std::printf("  UDP MD delivered %d/5 via %s\n", got, path);
  CHECK(got > 0);
}

// A session bound to a real account must force THAT account onto every command,
// so a client cannot act as another account by writing a different id into the
// payload. An unbound (account 0 / trusted-transport) session passes through.
void test_session_account_binding()
{
  std::printf("test_session_account_binding\n");
  auto dec = [](const uint8_t* p, size_t n)
  { return OuchCodec::decode(p, n); };

  // Bound to account 7; the wire order claims account 1 -> must be stamped to 7.
  GatewaySession bound(7, dec);
  bound.authenticate(true);
  std::vector<uint8_t> buf;
  OuchCodec::encode(InboundCommand{mk(1, Side::BUY, 100, 1, /*acct*/ 1)}, buf);
  SessionReject rej{};
  auto cmd = bound.handle(buf.data(), buf.size(), 1000, rej);
  CHECK(cmd.has_value());
  const auto* no = std::get_if<NewOrder>(&*cmd);
  CHECK(no != nullptr);
  CHECK(no->accountId == 7);  // client-supplied 1 overridden by the session account

  // Unbound session (account 0): the client-supplied account passes through.
  GatewaySession open(0, dec);
  open.authenticate(true);
  std::vector<uint8_t> buf2;
  OuchCodec::encode(InboundCommand{mk(2, Side::BUY, 100, 1, /*acct*/ 1)}, buf2);
  auto cmd2 = open.handle(buf2.data(), buf2.size(), 1000, rej);
  CHECK(cmd2.has_value());
  const auto* no2 = std::get_if<NewOrder>(&*cmd2);
  CHECK(no2 != nullptr && no2->accountId == 1);  // trusted passthrough
}

void test_logon()
{
  std::printf("test_logon\n");
  const std::string secret = "topsecret";
  auto dec = [](const uint8_t* p, size_t n)
  { return OuchCodec::decode(p, n); };
  const std::string apiKey = "key123";
  const uint64_t ts = 1000;
  const std::string payload = apiKey + ":" + std::to_string(ts);
  const auto mac = crypto::hmacSha256(reinterpret_cast<const uint8_t*>(secret.data()), secret.size(),
                                      reinterpret_cast<const uint8_t*>(payload.data()),
                                      payload.size());
  const std::string sig = crypto::toHex(mac.data(), mac.size());

  GatewaySession s(7, dec, flox::RateLimitPolicy::binance_um_futures(), secret);
  CHECK(!s.authenticated());
  CHECK(s.logon(apiKey, ts, sig, 1002));  // within skew
  CHECK(s.authenticated());

  GatewaySession s2(7, dec, flox::RateLimitPolicy::binance_um_futures(), secret);
  CHECK(!s2.logon(apiKey, ts, "deadbeef", 1002));  // bad signature
  CHECK(!s2.authenticated());

  GatewaySession s3(7, dec, flox::RateLimitPolicy::binance_um_futures(), secret);
  CHECK(!s3.logon(apiKey, ts, sig, 2000));  // skew 1000s > 5s
}

void test_websocket()
{
  std::printf("test_websocket\n");
  CHECK(ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  const std::string req =
      "GET /ws HTTP/1.1\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
  const std::string resp = ws::handshakeResponse(req);
  CHECK(resp.find("101 Switching Protocols") != std::string::npos);
  CHECK(resp.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

  const char* txt = "{\"a\":1}";
  auto f = ws::buildFrame(ws::Opcode::Text, reinterpret_cast<const uint8_t*>(txt), 7);
  ws::Opcode op{};
  std::vector<uint8_t> pl;
  CHECK(ws::parseFrame(f.data(), f.size(), op, pl) == f.size());
  CHECK(op == ws::Opcode::Text && std::string(pl.begin(), pl.end()) == "{\"a\":1}");

  // masked client frame ("abc")
  std::vector<uint8_t> mf = {0x81, static_cast<uint8_t>(0x80 | 3)};
  const uint8_t mask[4] = {1, 2, 3, 4};
  for (int i = 0; i < 4; ++i)
  {
    mf.push_back(mask[i]);
  }
  const char* abc = "abc";
  for (int i = 0; i < 3; ++i)
  {
    mf.push_back(static_cast<uint8_t>(abc[i] ^ mask[i & 3]));
  }
  CHECK(ws::parseFrame(mf.data(), mf.size(), op, pl) == mf.size());
  CHECK(std::string(pl.begin(), pl.end()) == "abc");

  // Hostile 64-bit extended length that OVERFLOWS off + len: 0x82 FIN+Binary,
  // 0x7F = length code 127, then len = 2^64 - 9. Naively `n < off + len` wraps to
  // a tiny value and the guard is bypassed -> resize(~2^64) crash. Must instead
  // be rejected as a protocol error (kParseError), never a crash or a 0.
  std::vector<uint8_t> ovf = {0x82, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7};
  CHECK(ws::parseFrame(ovf.data(), ovf.size(), op, pl) == ws::kParseError);

  // Oversized-but-not-overflowing length (above kMaxFramePayload) is also
  // rejected before allocation, not buffered forever.
  std::vector<uint8_t> big = {0x82, 0x7F, 0, 0, 0, 0, 0x10, 0, 0, 0};  // len = 0x10000000 = 256 MiB
  CHECK(ws::parseFrame(big.data(), big.size(), op, pl) == ws::kParseError);

  // A well-formed but not-yet-complete frame still returns 0 (need more bytes),
  // NOT an error: 4-byte payload declared, only 2 delivered.
  std::vector<uint8_t> partial = {0x82, 0x04, 0xAA, 0xBB};
  CHECK(ws::parseFrame(partial.data(), partial.size(), op, pl) == 0);

  // Binary/TLS length-prefix path: a u32 prefix beyond net::kMaxFrame must make
  // readFrame reject rather than resize() up to 4 GiB. Drive it through a pipe.
  {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    const uint8_t hostileHdr[4] = {0xFF, 0xFF, 0xFF, 0xFF};  // len = 4294967295
    CHECK(::write(fds[1], hostileHdr, 4) == 4);
    ::close(fds[1]);
    std::vector<uint8_t> out;
    CHECK(!net::readFrame(fds[0], out));  // rejected before allocation
    ::close(fds[0]);
  }
}

void test_control_server()
{
  std::printf("test_control_server\n");
  InstrumentRegistry reg;
  ControlApi api(reg);
  ControlServer srv(api);
  std::istringstream in(std::string(R"({"method":"listInstrument","symbol":1,"tick":0.01})") + "\n" +
                        R"({"method":"list"})" + "\n");
  std::ostringstream out;
  srv.serve(in, out);
  const std::string s = out.str();
  CHECK(s.find("\"ok\":true") != std::string::npos);
  CHECK(s.find("\"instruments\":[1]") != std::string::npos);
}

void test_tcp_control()
{
  std::printf("test_tcp_control\n");
  InstrumentRegistry reg;
  ControlApi api(reg);
  TcpControlServer srv(api);
  const int port = srv.start(0);
  CHECK(port > 0);

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a);
  timeval tv{1, 0};
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  auto request = [&](const std::string& req) -> std::string
  {
    ::write(c, req.data(), req.size());
    std::string resp;
    char tb[512];
    while (resp.find('\n') == std::string::npos)
    {
      const ssize_t r = ::read(c, tb, sizeof tb);
      if (r <= 0)
      {
        break;
      }
      resp.append(tb, static_cast<size_t>(r));
    }
    return resp;
  };

  CHECK(request(R"({"method":"listInstrument","symbol":1,"tick":0.01})"
                "\n")
            .find("\"ok\":true") != std::string::npos);
  CHECK(request(R"({"method":"list"})"
                "\n")
            .find("\"instruments\":[1]") != std::string::npos);
  ::close(c);
  srv.stop();
}

void test_md_snapshot()
{
  std::printf("test_md_snapshot\n");
  MarketDataPublisher<> md([](const MdMessage&) {}, Price::fromDouble(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e); });
  eng.submit(mk(1, Side::SELL, 100, 5));
  eng.submit(mk(2, Side::SELL, 101, 3));
  eng.submit(mk(3, Side::BUY, 99, 4));

  const auto snap = md.snapshot();
  CHECK(snap.size() == 3);
  Quantity ask100{}, bid99{};
  for (const auto& m : snap)
  {
    if (m.side == Side::SELL && m.price == px(100))
    {
      ask100 += m.qty;
    }
    if (m.side == Side::BUY && m.price == px(99))
    {
      bid99 += m.qty;
    }
  }
  CHECK(ask100 == qty(5));
  CHECK(bid99 == qty(4));
  CHECK(md.book().askAtPrice(px(100)) == qty(5));  // snapshot agrees with live depth
}

std::vector<uint8_t> wsClientFrame(const std::string& s)
{
  std::vector<uint8_t> f;
  f.push_back(0x81);  // FIN + Text
  const size_t n = s.size();
  const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
  if (n < 126)
  {
    f.push_back(static_cast<uint8_t>(0x80 | n));
  }
  else
  {
    f.push_back(0x80 | 126);
    f.push_back(static_cast<uint8_t>(n >> 8));
    f.push_back(static_cast<uint8_t>(n));
  }
  for (int i = 0; i < 4; ++i)
  {
    f.push_back(mask[i]);
  }
  for (size_t i = 0; i < n; ++i)
  {
    f.push_back(static_cast<uint8_t>(s[i] ^ mask[i & 3]));
  }
  return f;
}

void test_ws_gateway()
{
  std::printf("test_ws_gateway\n");
  std::mutex m;
  WsGateway::Responder currentResp;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     const std::string j = RestJson::encode(e);
                                     if (!j.empty() && currentResp){
                                       currentResp(reinterpret_cast<const uint8_t*>(j.data()), j.size());
} });
  WsGateway gw([](const uint8_t* p, size_t n)
               { return RestJson::decode(std::string(reinterpret_cast<const char*>(p), n)); });
  const int port = gw.start(0, [&](const InboundCommand& c, const WsGateway::Responder& r)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              currentResp = r;
                              eng.submit(c);
                              currentResp = {}; });
  CHECK(port > 0);

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a);
  timeval tv{1, 0};
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  const std::string hs =
      "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  ::write(c, hs.data(), hs.size());

  std::string resp;
  uint8_t tb[2048];
  while (resp.find("\r\n\r\n") == std::string::npos)
  {
    const ssize_t r = ::read(c, tb, sizeof tb);
    if (r <= 0)
    {
      break;
    }
    resp.append(reinterpret_cast<char*>(tb), static_cast<size_t>(r));
  }
  CHECK(resp.find("101 Switching Protocols") != std::string::npos);

  auto sell = wsClientFrame(
      R"({"action":"new","id":1,"symbol":1,"side":"sell","ordType":"limit","qty":5,"price":100})");
  ::write(c, sell.data(), sell.size());
  auto buy = wsClientFrame(
      R"({"action":"new","id":2,"symbol":1,"side":"buy","ordType":"limit","qty":3,"price":100,"account":2})");
  ::write(c, buy.data(), buy.size());

  std::vector<uint8_t> buf;
  std::string all;
  for (int iter = 0; iter < 50 && all.find("\"type\":\"trade\"") == std::string::npos; ++iter)
  {
    const ssize_t r = ::read(c, tb, sizeof tb);
    if (r <= 0)
    {
      break;
    }
    buf.insert(buf.end(), tb, tb + r);
    ws::Opcode op{};
    std::vector<uint8_t> pl;
    size_t cons;
    while ((cons = ws::parseFrame(buf.data(), buf.size(), op, pl)) > 0)
    {
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cons));
      if (op == ws::Opcode::Text)
      {
        all.append(pl.begin(), pl.end());
      }
    }
  }
  ::close(c);
  gw.stop();
  CHECK(all.find("\"type\":\"trade\"") != std::string::npos);  // WS round-trip produced a trade
}

void test_cancel_on_disconnect()
{
  std::printf("test_cancel_on_disconnect\n");
  std::mutex m;
  TcpGateway::Responder currentResp;
  std::vector<OutboundEvent> events;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     events.push_back(e);
                                     std::vector<uint8_t> b;
                                     OuchCodec::encode(e, b);
                                     if (!b.empty() && currentResp){ currentResp(b.data(), b.size());
} });

  TcpGateway gw([](const uint8_t* p, size_t n)
                { return OuchCodec::decode(p, n); });
  gw.setCancelOnDisconnect(true);
  const int port = gw.start(0, [&](const InboundCommand& c, const TcpGateway::Responder& r)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              currentResp = r;
                              eng.submit(c);
                              currentResp = {}; });
  CHECK(port > 0);

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a);

  std::vector<uint8_t> b;
  OuchCodec::encode(InboundCommand{mk(1, Side::SELL, 100, 5, 3)}, b);
  net::writeFrame(c, b.data(), b.size());

  timeval tv{1, 0};
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::vector<uint8_t> frame;
  net::readFrame(c, frame);  // wait for the Accepted report -> order is resting
  ::close(c);
  gw.stop();  // joins the conn thread -> disconnect -> cancel-on-disconnect fires

  CHECK(eng.book().empty());  // the resting order was canceled on disconnect
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

void test_ws_cancel_on_disconnect()
{
  std::printf("test_ws_cancel_on_disconnect\n");
  std::mutex m;
  WsGateway::Responder currentResp;
  std::vector<OutboundEvent> events;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     events.push_back(e);
                                     const std::string j = RestJson::encode(e);
                                     if (!j.empty() && currentResp){
                                       currentResp(reinterpret_cast<const uint8_t*>(j.data()), j.size());
} });
  WsGateway gw([](const uint8_t* p, size_t n)
               { return RestJson::decode(std::string(reinterpret_cast<const char*>(p), n)); });
  gw.setCancelOnDisconnect(true);
  const int port = gw.start(0, [&](const InboundCommand& c, const WsGateway::Responder& r)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              currentResp = r;
                              eng.submit(c);
                              currentResp = {}; });
  CHECK(port > 0);

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a);
  timeval tv{1, 0};
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  const std::string hs =
      "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  ::write(c, hs.data(), hs.size());
  std::string resp;
  uint8_t tb[2048];
  while (resp.find("\r\n\r\n") == std::string::npos)
  {
    const ssize_t r = ::read(c, tb, sizeof tb);
    if (r <= 0)
    {
      break;
    }
    resp.append(reinterpret_cast<char*>(tb), static_cast<size_t>(r));
  }

  auto sell = wsClientFrame(
      R"({"action":"new","id":1,"symbol":1,"side":"sell","ordType":"limit","qty":5,"price":100,"account":3})");
  ::write(c, sell.data(), sell.size());

  std::vector<uint8_t> buf;
  bool accepted = false;
  for (int iter = 0; iter < 50 && !accepted; ++iter)
  {
    const ssize_t r = ::read(c, tb, sizeof tb);
    if (r <= 0)
    {
      break;
    }
    buf.insert(buf.end(), tb, tb + r);
    ws::Opcode op{};
    std::vector<uint8_t> pl;
    size_t cons;
    while ((cons = ws::parseFrame(buf.data(), buf.size(), op, pl)) > 0)
    {
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cons));
      if (op == ws::Opcode::Text &&
          std::string(pl.begin(), pl.end()).find("accepted") != std::string::npos)
      {
        accepted = true;
      }
    }
  }
  CHECK(accepted);  // order is resting
  ::close(c);
  gw.stop();  // disconnect -> cancel-on-disconnect fires over WebSocket

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

}  // namespace

TEST(Network, EngineSuite)
{
  test_session();
  test_session_account_binding();
  test_logon();
  test_websocket();
  test_control_server();
  test_tcp_control();
  test_md_snapshot();
  test_tcp_gateway();
  test_ws_gateway();
  test_cancel_on_disconnect();
  test_ws_cancel_on_disconnect();
  test_udp_md();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
