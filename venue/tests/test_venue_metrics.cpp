/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/ledger.h"
#include "flox-venue/messages.h"
#include "flox-venue/metrics.h"
#include "flox-venue/metrics_server.h"
#include "flox-venue/prometheus.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <atomic>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>

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

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

Metrics sampleMetrics()
{
  Metrics m;
  OrderAccepted acc{};
  acc.id = 1;
  m.observe(OutboundEvent{acc});
  Trade t{};
  t.price = px(100);
  t.quantity = qty(3);
  m.observe(OutboundEvent{t});
  OrderCanceled c{};
  c.id = 1;
  m.observe(OutboundEvent{c});
  OrderRejected rej{};
  rej.id = 9;
  rej.reason = RejectReason::PositionLimitExceeded;
  m.observe(OutboundEvent{rej});
  m.submitLatency.record(1000);
  m.submitLatency.record(5000);
  m.submitLatency.record(20000);
  return m;
}

bool contains(const std::string& hay, const std::string& needle)
{
  return hay.find(needle) != std::string::npos;
}

void test_renderer()
{
  std::printf("test_prometheus_renderer\n");
  const Metrics m = sampleMetrics();
  const std::string txt = prom::render(m);

  CHECK(contains(txt, "# TYPE fme_trades_total counter"));
  CHECK(contains(txt, "fme_trades_total 1"));
  CHECK(contains(txt, "fme_orders_accepted_total 1"));
  CHECK(contains(txt, "fme_cancels_total 1"));
  CHECK(contains(txt, "# TYPE fme_submit_latency_ns summary"));
  CHECK(contains(txt, "fme_submit_latency_ns{quantile=\"0.5\"}"));
  CHECK(contains(txt, "fme_submit_latency_ns_count 3"));
  CHECK(contains(txt, "fme_rejects_by_reason_total{reason=\"PositionLimitExceeded\"} 1"));
  // 3 fills @ 3 qty @ 100 -> traded notional raw = 300 * 1e8 = 30000000000
  CHECK(contains(txt, "fme_traded_volume_raw 30000000000"));

  // i128 helpers: negative and large values.
  CHECK(prom::i128ToStr(-50) == "-50");
  CHECK(prom::i128ToStr(0) == "0");
  unsigned __int128 big = static_cast<unsigned __int128>(1) << 100;
  CHECK(prom::u128ToStr(big) == "1267650600228229401496703205376");
}

void test_gauges()
{
  std::printf("test_prometheus_gauges\n");
  const Metrics m = sampleMetrics();
  Gauges g;
  g.insuranceFundRaw = amountOf(Volume::fromDouble(50000));
  g.fundingRate = 0.0001;
  g.openInterestRaw = amountOf(Volume::fromDouble(1200000));
  g.openPositions = 7;
  g.restingOrders = 42;
  g.markPriceAgeNs = 250000000;  // 250ms feed lag
  g.liquidationsPaused = 1;
  const std::string txt = prom::render(m, g);

  // Counters still present, plus the gauge blocks.
  CHECK(contains(txt, "fme_trades_total 1"));
  CHECK(contains(txt, "# TYPE fme_insurance_fund_raw gauge"));
  CHECK(contains(txt, "fme_insurance_fund_raw 5000000000000"));  // 50000 * 1e8
  CHECK(contains(txt, "# TYPE fme_funding_rate gauge"));
  CHECK(contains(txt, "fme_open_positions 7"));
  CHECK(contains(txt, "fme_resting_orders 42"));
  CHECK(contains(txt, "fme_mark_price_age_ns 250000000"));
  CHECK(contains(txt, "# TYPE fme_liquidations_paused gauge"));
  CHECK(contains(txt, "fme_liquidations_paused 1"));
}

std::string httpGet(int port, const char* path, std::string& status)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
  {
    ::close(fd);
    status = "CONNFAIL";
    return {};
  }
  std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
  ::write(fd, req.data(), req.size());
  timeval tv{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::string resp;
  char buf[4096];
  for (;;)
  {
    const ssize_t r = ::read(fd, buf, sizeof buf);
    if (r <= 0)
    {
      break;
    }
    resp.append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  const size_t nl = resp.find("\r\n");
  status = (nl == std::string::npos) ? resp : resp.substr(0, nl);
  const size_t body = resp.find("\r\n\r\n");
  return body == std::string::npos ? std::string{} : resp.substr(body + 4);
}

void test_server()
{
  std::printf("test_metrics_server\n");
  const Metrics m = sampleMetrics();
  std::atomic<bool> ready{false};
  MetricsServer srv([&]
                    { return prom::render(m); }, [&]
                    { return ready.load(); });
  const int port = srv.start(0);
  CHECK(port > 0);

  std::string st;
  const std::string metrics = httpGet(port, "/metrics", st);
  CHECK(contains(st, "200 OK"));
  CHECK(contains(metrics, "fme_trades_total 1"));

  const std::string health = httpGet(port, "/healthz", st);
  CHECK(contains(st, "200 OK"));
  CHECK(contains(health, "ok"));

  // Not ready yet -> 503.
  httpGet(port, "/readyz", st);
  CHECK(contains(st, "503"));

  // Flip to ready -> 200.
  ready.store(true);
  const std::string rz = httpGet(port, "/readyz", st);
  CHECK(contains(st, "200 OK"));
  CHECK(contains(rz, "ready"));

  // Unknown path -> 404.
  httpGet(port, "/nope", st);
  CHECK(contains(st, "404"));

  srv.stop();
}

}  // namespace

TEST(Metrics, EngineSuite)
{
  test_renderer();
  test_gauges();
  test_server();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
