/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/resend_buffer.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
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
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.lastLookWindowNs = 1000;  // exercise time-driven behaviour under replay
  return c;
}
NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct)
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

void test_resend()
{
  std::printf("test_resend\n");
  ResendBuffer buf;
  const uint64_t sess = 42;
  for (int i = 1; i <= 5; ++i)
  {
    const uint64_t seq =
        buf.append(sess, OutboundEvent{OrderCanceled{static_cast<OrderId>(i), SYM, CancelReason::UserRequested}},
                   /*ts*/ i * 100);
    CHECK(seq == static_cast<uint64_t>(i));
  }
  CHECK(buf.lastSeq(sess) == 5);

  // Reconnect: client had seen through seq 2, requests resend from 3.
  const auto gap = buf.resend(sess, 3);
  CHECK(gap.size() == 3);
  CHECK(gap.front().seq == 3 && gap.back().seq == 5);
  CHECK(gap.front().tsNs == 300);

  buf.ackThrough(sess, 4);
  CHECK(buf.resend(sess, 1).size() == 1);  // only seq 5 remains buffered

  // Client-side gap detection.
  GapDetector gd;
  CHECK(!gd.observe(1).first);
  CHECK(!gd.observe(2).first);
  auto g = gd.observe(5);  // skipped 3,4
  CHECK(g.first && g.second == 3);
}

uint64_t runTimed(const std::vector<std::pair<int64_t, InboundCommand>>& cmds)
{
  uint64_t h = 1469598103934665603ULL;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { h = hashEvent(h, e); });
  for (const auto& [ts, c] : cmds)
  {
    eng.submit(c, ts);
  }
  return h;
}

void test_timestamped_replay()
{
  std::printf("test_timestamped_replay\n");
  const std::string path = "/tmp/flox_test_venue_session_session_journal.bin";
  // A last-look maker + a taker hit; the hold times out at ts 5000 (window 1000).
  std::vector<std::pair<int64_t, InboundCommand>> live;
  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  live.emplace_back(10, InboundCommand{mk});
  live.emplace_back(20, InboundCommand{limit(2, Side::BUY, 100, 3, 2)});  // held @ deadline 1020
  live.emplace_back(5000, InboundCommand{CancelOrder{999, SYM, 1}});      // past deadline -> timeout

  {
    Journal j(path);
    for (const auto& [ts, c] : live)
    {
      j.append(c, ts);
    }
    j.flush();
  }
  const uint64_t hLive = runTimed(live);

  const auto replayed = Journal::loadTimed(path);
  CHECK(replayed.size() == live.size());
  CHECK(replayed[1].first == 20);  // timestamp preserved
  const uint64_t hReplay = runTimed(replayed);
  CHECK(hLive == hReplay);  // time-driven (last-look timeout) reproduced exactly
}

}  // namespace

TEST(Session, EngineSuite)
{
  test_resend();
  test_timestamped_replay();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
