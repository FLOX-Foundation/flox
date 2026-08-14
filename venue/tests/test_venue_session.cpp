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
#include "flox-venue/sbe_order_entry_codec.h"
#include "flox-venue/session_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// Session-layer resend now lives in SessionRegistry (the ResendBuffer event
// log was removed): the per-account stream assigns the seq at serialization
// time and retains the framed bytes, so a replay repeats the original reports
// byte-for-byte -- across a disconnect.
void test_resend()
{
  std::printf("test_resend\n");
  SessionRegistry reg(DeliveryConfig{/*queueCapacity*/ 64, /*resendLogCapacity*/ 4});
  const uint64_t acct = 42;
  std::mutex m;
  std::vector<std::vector<uint8_t>> wire;
  auto frames = [&]
  {
    std::lock_guard<std::mutex> lk(m);
    return wire;
  };
  auto waitFrames = [&](size_t want)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (frames().size() < want && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return frames().size() >= want;
  };
  const SessionRegistry::Encoder enc =
      [](const OutboundEvent& e, uint64_t seq, int64_t, std::vector<uint8_t>& out)
  {
    SbeOrderEntryCodec::encode(e, out, seq);
    return !out.empty();
  };
  auto attach = [&]
  {
    return reg.attach(acct, enc, [&](const uint8_t* p, size_t n)
                      {
                        std::lock_guard<std::mutex> lk(m);
                        wire.emplace_back(p, p + n);
                        return true; }, [] {});
  };

  auto w = attach();
  for (int i = 1; i <= 5; ++i)
  {
    reg.route(OutboundEvent{
        OrderCanceled{static_cast<OrderId>(i), SYM, CancelReason::UserRequested, acct}});
  }
  CHECK(reg.lastSeq(acct) == 5);
  CHECK(waitFrames(5));
  {
    const auto fs = frames();
    CHECK(SbeOrderEntryCodec::seqOf(fs.front().data(), fs.front().size()) == 1);
    CHECK(SbeOrderEntryCodec::seqOf(fs.back().data(), fs.back().size()) == 5);
  }

  // Disconnect; an event that fires while the account is offline is still
  // sequenced and logged -- exactly the maker-fill-during-reconnect case.
  reg.detach(acct, w);
  w->stop();
  reg.route(OutboundEvent{OrderCanceled{6, SYM, CancelReason::UserRequested, acct}});
  CHECK(reg.lastSeq(acct) == 6);

  // Reconnect: client had seen through seq 3, requests resend from 4. Log
  // capacity is 4, so seqs 3..6 are retained -> served.
  {
    std::lock_guard<std::mutex> lk(m);
    wire.clear();
  }
  auto w2 = attach();
  CHECK(reg.resendFrom(acct, 4) == SessionRegistry::ResendResult::Served);
  CHECK(waitFrames(3));
  {
    const auto fs = frames();
    CHECK(fs.size() == 3);
    CHECK(SbeOrderEntryCodec::seqOf(fs.front().data(), fs.front().size()) == 4);
    CHECK(SbeOrderEntryCodec::seqOf(fs.back().data(), fs.back().size()) == 6);
  }

  // A fromSeq older than the retained log is an explicit TooOld (the verb
  // layer answers SnapshotRequired), never a silent hole.
  CHECK(reg.resendFrom(acct, 1) == SessionRegistry::ResendResult::TooOld);
  reg.detach(acct, w2);
  w2->stop();

  // Client-side gap detection (message-level, in-order delivery; the full
  // reordering / re-raise / epoch semantics are covered in the MD tests).
  GapDetector gd;
  std::vector<uint64_t> delivered;
  std::vector<uint64_t> gaps;
  auto deliver = [&](const MdMessage& d)
  { delivered.push_back(d.seq); };
  auto onGap = [&](SymbolId, uint64_t, uint64_t from)
  { gaps.push_back(from); };
  auto msg = [](uint64_t seq)
  {
    MdMessage d;
    d.symbol = SYM;
    d.epoch = 1;
    d.seq = seq;
    return d;
  };
  gd.observe(msg(1), deliver, onGap);
  gd.observe(msg(2), deliver, onGap);
  gd.observe(msg(5), deliver, onGap);  // skipped 3,4 -> gap from 3; 5 is held
  CHECK(gaps.size() == 1 && gaps[0] == 3);
  CHECK(delivered.size() == 2);
  gd.observe(msg(3), deliver, onGap);  // late arrival fills part of the gap -> 3 delivers
  gd.observe(msg(4), deliver, onGap);  // gap closed -> 4 and the held 5 drain in order
  CHECK((delivered == std::vector<uint64_t>{1, 2, 3, 4, 5}));
  CHECK(gaps.size() == 1);
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
