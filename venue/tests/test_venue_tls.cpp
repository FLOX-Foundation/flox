/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/ouch_codec.h"
#include "flox-venue/tls_gateway.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
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
                                     OuchCodec::encode(e, b);
                                     if (!b.empty() && currentResp){ currentResp(b.data(), b.size());
} });

  TlsGateway gw([](const uint8_t* p, size_t n)
                { return OuchCodec::decode(p, n); });
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
    OuchCodec::encode(c, b);
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
      tags.insert(frame[0]);
    }
    if (tags.count(uint8_t(OuchOut::Accepted)) && tags.count(uint8_t(OuchOut::Trade)) &&
        tags.count(uint8_t(OuchOut::Executed)))
    {
      break;
    }
  }
  SSL_shutdown(ssl);
  SSL_free(ssl);
  ::close(fd);
  SSL_CTX_free(cctx);
  gw.stop();

  CHECK(tags.count(uint8_t(OuchOut::Accepted)) == 1);
  CHECK(tags.count(uint8_t(OuchOut::Trade)) == 1);
  CHECK(tags.count(uint8_t(OuchOut::Executed)) == 1);
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
                                     OuchCodec::encode(e, b);
                                     if (!b.empty() && currentResp){ currentResp(b.data(), b.size());
} });
  TlsGateway gw([](const uint8_t* p, size_t n)
                { return OuchCodec::decode(p, n); });
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
  OuchCodec::encode(InboundCommand{mk(1, Side::SELL, 100, 5, 1)}, b);
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

}  // namespace

TEST(Tls, EngineSuite)
{
  test_tls_roundtrip();
  test_tls_cancel_on_disconnect();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
