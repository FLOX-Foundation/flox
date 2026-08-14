/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Liveness and cancel-on-disconnect:
 *  - a black-holed peer (connect, order, silence) is disconnected on the idle
 *    timeout and COD sweeps its orders -- previously a half-open peer held its
 *    orders and a gateway thread forever;
 *  - stop() completes promptly with a silent client attached (the acceptor
 *    shuts every connection fd down before joining);
 *  - the WebSocket path pings at half the idle window and drops a peer that
 *    never answers;
 *  - DisconnectCanceller prunes terminally-resolved orders, so a disconnect
 *    flush never cancels an order that already filled.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/rest_json.h"
#include "flox-venue/sbe_order_entry_codec.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/tcp_gateway.h"
#include "flox-venue/ws_gateway.h"
#include "flox/util/transport.h"
#include "flox/util/websocket.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
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

NewOrder mk(OrderId id, Side s, double p, double q, uint64_t acct = 0)
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

int tcpConnect(int port)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
  {
    ::close(fd);
    return -1;
  }
  timeval tv{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

bool waitUntil(const std::function<bool()>& pred, int ms = 3000)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (pred())
    {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// Black-holed peer: connect, place an order, go silent. The idle timeout must
// close the session and COD must pull the resting order.
void test_idle_disconnect_cod()
{
  std::printf("test_idle_disconnect_cod\n");
  GatewayCounters counters;
  SessionRegistry registry({}, &counters);
  std::mutex m;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { registry.route(e); });

  TcpGateway gw([](const uint8_t* p, size_t n)
                { return SbeOrderEntryCodec::decode(p, n); },
                /*account*/ 1);
  gw.setDelivery(&registry, [](const OutboundEvent& e, uint64_t seq, int64_t, std::vector<uint8_t>& out)
                 {
                   SbeOrderEntryCodec::encode(e, out, seq);
                   return !out.empty(); });
  gw.setCounters(&counters);
  gw.setCancelOnDisconnect(true);
  gw.setIdleTimeout(std::chrono::milliseconds(300));
  const int port = gw.start(0, [&](const InboundCommand& c, const TcpGateway::Responder&)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              eng.submit(c); });
  CHECK(port > 0);

  const int c = tcpConnect(port);
  CHECK(c >= 0);
  std::vector<uint8_t> b;
  SbeOrderEntryCodec::encode(InboundCommand{mk(1, Side::SELL, 100, 5)}, b);
  net::writeFrame(c, b.data(), b.size());
  std::vector<uint8_t> frame;
  CHECK(net::readFrame(c, frame));  // Accepted: the order is resting

  // Silence. The idle window (300ms) elapses -> the venue closes the session
  // and cancel-on-disconnect sweeps the book.
  CHECK(waitUntil([&]
                  {
                    std::lock_guard<std::mutex> lk(m);
                    return eng.book().empty(); }));
  CHECK(counters.idleDisconnects.load() >= 1);
  CHECK(!net::readFrame(c, frame) || true);  // drain; the peer socket is dead either way
  ::close(c);
  gw.stop();
}

// stop() must complete promptly while a silent client is connected: the
// acceptor's shutdown sweep unblocks the parked read.
void test_stop_with_silent_client()
{
  std::printf("test_stop_with_silent_client\n");
  TcpGateway gw([](const uint8_t* p, size_t n)
                { return SbeOrderEntryCodec::decode(p, n); });
  const int port = gw.start(0, [](const InboundCommand&, const TcpGateway::Responder&) {});
  CHECK(port > 0);
  const int c = tcpConnect(port);
  CHECK(c >= 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the conn thread park in read

  const auto t0 = std::chrono::steady_clock::now();
  gw.stop();  // idle timeout is 30s: without the sweep this would hang that long
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  std::printf("  stop() with silent client took %lldms\n", static_cast<long long>(ms));
  CHECK(ms < 2000);
  ::close(c);
}

// WebSocket liveness: the server pings at half the idle window; a peer that
// answers nothing is dropped after the full window.
void test_ws_idle_disconnect()
{
  std::printf("test_ws_idle_disconnect\n");
  GatewayCounters counters;
  WsGateway gw([](const uint8_t* p, size_t n)
               { return RestJson::decode(std::string(reinterpret_cast<const char*>(p), n)); });
  gw.setCounters(&counters);
  gw.setIdleTimeout(std::chrono::milliseconds(400));
  const int port = gw.start(0, [](const InboundCommand&, const WsGateway::Responder&) {});
  CHECK(port > 0);

  const int c = tcpConnect(port);
  CHECK(c >= 0);
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

  // Read whatever the server sends (Pings) without ever answering; the server
  // must close the connection once the idle window passes.
  bool sawPing = false;
  bool closed = false;
  std::vector<uint8_t> buf;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline)
  {
    const ssize_t r = ::read(c, tb, sizeof tb);
    if (r == 0)
    {
      closed = true;
      break;
    }
    if (r < 0)
    {
      continue;  // client-side read timeout tick
    }
    buf.insert(buf.end(), tb, tb + r);
    ws::Opcode op{};
    std::vector<uint8_t> pl;
    size_t cons;
    while ((cons = ws::parseFrame(buf.data(), buf.size(), op, pl)) > 0 && cons != ws::kParseError)
    {
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cons));
      sawPing |= (op == ws::Opcode::Ping);
    }
  }
  CHECK(sawPing);  // keepalive probe went out at half the window
  CHECK(closed);   // and the unresponsive peer was dropped
  CHECK(counters.idleDisconnects.load() >= 1);
  ::close(c);
  gw.stop();
}

// COD pruning: an order that terminally resolved (full fill) while the session
// was up must NOT be canceled by the disconnect flush.
void test_cod_prunes_filled_orders()
{
  std::printf("test_cod_prunes_filled_orders\n");
  GatewayCounters counters;
  SessionRegistry registry({}, &counters);
  std::mutex m;
  int cancelCmds = 0;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { registry.route(e); });

  TcpGateway gw([](const uint8_t* p, size_t n)
                { return SbeOrderEntryCodec::decode(p, n); },
                /*account*/ 1);
  gw.setDelivery(&registry, [](const OutboundEvent& e, uint64_t seq, int64_t, std::vector<uint8_t>& out)
                 {
                   SbeOrderEntryCodec::encode(e, out, seq);
                   return !out.empty(); });
  gw.setCancelOnDisconnect(true);
  const int port = gw.start(0, [&](const InboundCommand& c, const TcpGateway::Responder&)
                            {
                              std::lock_guard<std::mutex> lk(m);
                              if (std::get_if<CancelOrder>(&c) != nullptr)
                              {
                                ++cancelCmds;
                              }
                              eng.submit(c); });
  CHECK(port > 0);

  const int c = tcpConnect(port);
  CHECK(c >= 0);
  std::vector<uint8_t> b;
  SbeOrderEntryCodec::encode(InboundCommand{mk(1, Side::SELL, 100, 5)}, b);
  net::writeFrame(c, b.data(), b.size());
  std::vector<uint8_t> frame;
  CHECK(net::readFrame(c, frame));  // Accepted

  // A foreign aggressor fills the whole order: terminal -> COD must untrack it.
  {
    std::lock_guard<std::mutex> lk(m);
    eng.submit(InboundCommand{mk(2, Side::BUY, 100, 5, /*acct*/ 2)});
  }
  // Wait until the terminal report was routed (the observer pruned on route).
  CHECK(waitUntil([&]
                  { return registry.lastSeq(1) >= 3; }));  // Accepted, Trade, Executed

  ::close(c);  // disconnect
  gw.stop();

  std::lock_guard<std::mutex> lk(m);
  CHECK(cancelCmds == 0);  // no cancel for the long-gone order
  CHECK(eng.book().empty());
}

}  // namespace

TEST(Liveness, EngineSuite)
{
  test_idle_disconnect_cod();
  test_stop_with_silent_client();
  test_ws_idle_disconnect();
  test_cod_prunes_filled_orders();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
