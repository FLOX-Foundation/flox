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
#include "flox-venue/ledger.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/sbe_order_entry_codec.h"
#include "flox-venue/session_registry.h"

#include "flox/backtest/fee_schedule.h"

#include <gtest/gtest.h>
#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 100;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount base(double v) { return amountOf(qty(v)); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50);
  c.maxPrice = px(150);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
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

// Balance genesis as journaled commands: every replay below starts from an
// EMPTY ledger and rebuilds balances from the stream itself.
std::vector<std::pair<int64_t, InboundCommand>> genesis()
{
  std::vector<std::pair<int64_t, InboundCommand>> g;
  int64_t ts = 1;
  for (uint64_t a = 1; a <= 3; ++a)
  {
    g.emplace_back(ts++, InboundCommand{Deposit{a, BASE, static_cast<int64_t>(base(1000)), SYM}});
    g.emplace_back(ts++, InboundCommand{Deposit{a, QUOTE, static_cast<int64_t>(quote(100000)), SYM}});
  }
  return g;
}

// A representative session of order flow (crossing pairs + a cancel).
std::vector<std::pair<int64_t, InboundCommand>> session()
{
  std::vector<std::pair<int64_t, InboundCommand>> s;
  s.emplace_back(10, InboundCommand{limit(1, Side::SELL, 100, 5, 1)});
  s.emplace_back(20, InboundCommand{limit(2, Side::SELL, 101, 3, 1)});
  s.emplace_back(30, InboundCommand{limit(3, Side::BUY, 99, 4, 2)});
  s.emplace_back(40, InboundCommand{limit(4, Side::BUY, 100, 6, 2)});  // trades 5@100, rests 1@100
  s.emplace_back(50, InboundCommand{CancelOrder{2, SYM, 1}});
  s.emplace_back(60, InboundCommand{limit(5, Side::SELL, 100, 2, 3)});  // trades 1@100 vs id4
  return s;
}

// Replay from an EMPTY ledger: deposits are part of `cmds` (genesis in the
// WAL), so no manual seeding happens here.
uint64_t runReplay(const std::vector<std::pair<int64_t, InboundCommand>>& cmds, Ledger& led)
{
  uint64_t h = 1469598103934665603ULL;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { h = hashEvent(h, e); });
  flox::FeeSchedule fs;
  fs.addTier(0.0, -1.0, 2.0);
  eng.setFeeSchedule(fs);
  eng.setLedger(&led, VENUE);
  for (const auto& [ts, c] : cmds)
  {
    eng.submit(c, ts);
  }
  return h;
}

// An auction driven through the command stream (AdminCmd) must survive journal
// replay: the uncross fills are produced in BOTH the live run and the replay, so
// a crash after an opening uncross recovers an identical book/ledger. If AdminCmd
// were not journaled+dispatched, the uncross would be lost on replay (divergence).
void test_auction_recovery()
{
  std::printf("test_auction_recovery\n");
  const std::string jpath = "/tmp/flox_test_venue_venue_auction_journal.bin";
  std::vector<std::pair<int64_t, InboundCommand>> cmds = genesis();  // deposits in the stream
  cmds.emplace_back(10, InboundCommand{AdminCmd{SYM, AdminAction::BeginPreOpen}});
  cmds.emplace_back(20, InboundCommand{limit(1, Side::SELL, 100, 5, 1)});             // accumulate (no match)
  cmds.emplace_back(30, InboundCommand{limit(2, Side::BUY, 100, 5, 2)});              // crossed book in pre-open
  cmds.emplace_back(40, InboundCommand{AdminCmd{SYM, AdminAction::OpenContinuous}});  // uncross @100

  // Live run: journal every command, hash the event stream, count trades.
  // The ledger starts empty; the genesis deposits fund it through the stream.
  Ledger liveLed;
  Journal jr(jpath);
  uint64_t liveHash = 1469598103934665603ULL;
  int liveTrades = 0;
  MatchingEngine<MatchingBook> leng(cfg(), [&](const OutboundEvent& e)
                                    { liveHash = hashEvent(liveHash, e);
                                      if (std::get_if<Trade>(&e)){ ++liveTrades;
} });
  leng.setLedger(&liveLed, VENUE);
  for (auto& [ts, c] : cmds)
  {
    jr.append(c, ts);
    leng.submit(c, ts);
  }
  jr.flush();
  CHECK(liveTrades > 0);  // the uncross produced fills -> AdminCmd(OpenContinuous) dispatched

  // Recovery: replay the journal into a fresh engine and an EMPTY ledger;
  // state must match bit-for-bit (deposits replay from the WAL).
  const auto replayed = Journal::loadTimed(jpath);
  CHECK(replayed.size() == cmds.size());  // AdminCmds round-tripped (not truncated at an unknown tag)
  Ledger recLed;
  CHECK(recLed.available(1, BASE) == 0);  // nothing seeded before replay
  uint64_t recHash = 1469598103934665603ULL;
  int recTrades = 0;
  MatchingEngine<MatchingBook> reng(cfg(), [&](const OutboundEvent& e)
                                    { recHash = hashEvent(recHash, e);
                                      if (std::get_if<Trade>(&e)){ ++recTrades;
} });
  reng.setLedger(&recLed, VENUE);
  for (auto& [ts, c] : replayed)
  {
    reng.submit(c, ts);
  }
  CHECK(recHash == liveHash);  // replay reproduces the auction uncross exactly
  CHECK(recTrades == liveTrades);
  // Ledger conserved and identical post-uncross.
  CHECK(liveLed.available(1, BASE) == recLed.available(1, BASE));
  CHECK(liveLed.available(2, QUOTE) == recLed.available(2, QUOTE));
}

}  // namespace

TEST(Venue, EngineSuite)
{
  std::printf("test_venue\n");
  test_auction_recovery();

  Ledger led;
  const Amount initBase = base(1000) * 3;
  const Amount initQuote = quote(100000) * 3;

  Metrics metrics;
  // Delivery/resend now lives in SessionRegistry: per-account seq + frame log,
  // replayable via resendFrom (what the SBE ResendRequest verb serves).
  SessionRegistry registry(DeliveryConfig{4096, 4096});
  std::mutex wiresMutex;
  std::unordered_map<uint64_t, std::vector<std::vector<uint8_t>>> wires;
  const SessionRegistry::Encoder enc =
      [](const OutboundEvent& e, uint64_t seq, int64_t, std::vector<uint8_t>& out)
  {
    SbeOrderEntryCodec::encode(e, out, seq);
    return !out.empty();
  };
  auto attachAccount = [&](uint64_t a)
  {
    return registry.attach(a, enc, [&wiresMutex, &wires, a](const uint8_t* fp, size_t fn)
                           {
                             std::lock_guard<std::mutex> lk(wiresMutex);
                             wires[a].emplace_back(fp, fp + fn);
                             return true; }, [] {});
  };
  std::vector<std::shared_ptr<SessionWriter>> writers;
  for (uint64_t a = 1; a <= 3; ++a)
  {
    writers.push_back(attachAccount(a));
  }
  auto frameCount = [&](uint64_t a)
  {
    std::lock_guard<std::mutex> lk(wiresMutex);
    return wires[a].size();
  };
  auto waitFrames = [&](uint64_t a, size_t want)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (frameCount(a) < want && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return frameCount(a) >= want;
  };
  MarketDataPublisher<> md([](const MdMessage&) {}, px(0.01), SYM);
  const std::string journalPath = "/tmp/flox_test_venue_venue_venue_journal.bin";
  Journal journal(journalPath);
  uint64_t liveHash = 1469598103934665603ULL;

  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     metrics.observe(e);
                                     md.onEvent(e);
                                     registry.route(e);
                                     liveHash = hashEvent(liveHash, e); });
  flox::FeeSchedule fs;
  fs.addTier(0.0, -1.0, 2.0);
  eng.setFeeSchedule(fs);
  eng.setLedger(&led, VENUE);

  // Genesis deposits enter as sequenced commands (journaled like everything
  // else). They are not order-entry wire, so they bypass the SBE codec: an ops
  // channel, not a client session, originates them.
  const auto gen = genesis();
  for (const auto& [ts, cmd] : gen)
  {
    eng.submit(cmd, ts);
    journal.append(cmd, ts);
  }

  // Drive the session through the SBE order-entry wire codec, timing each submit.
  const auto cmds = session();
  for (const auto& [ts, cmd] : cmds)
  {
    std::vector<uint8_t> wire;
    SbeOrderEntryCodec::encode(cmd, wire);
    auto decoded = SbeOrderEntryCodec::decode(wire.data(), wire.size());
    CHECK(decoded.has_value());
    const auto t0 = std::chrono::steady_clock::now();
    eng.submit(*decoded, ts);
    const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    metrics.submitLatency.record(static_cast<uint64_t>(dt));
    journal.append(cmd, ts);
  }
  journal.flush();

  // 1. Market data top-of-book matches the engine book.
  CHECK(eng.book().bestBid() == md.book().bestBid());
  CHECK(eng.book().bestAsk() == md.book().bestAsk());

  // 2. Value conserved through settlement + fees.
  Amount sumBase = led.total(VENUE, BASE);
  Amount sumQuote = led.total(VENUE, QUOTE);
  for (int a = 1; a <= 3; ++a)
  {
    sumBase += led.total(a, BASE);
    sumQuote += led.total(a, QUOTE);
  }
  CHECK(sumBase == initBase);
  CHECK(sumQuote == initQuote);

  // 3. Metrics: latency recorded per submit, trades counted.
  CHECK(metrics.submitLatency.count() == cmds.size());
  CHECK(metrics.trades > 0);
  CHECK(metrics.submitLatency.percentileNs(0.5) > 0);
  std::printf("  submit latency p50=%lluns p99=%lluns; trades=%llu, volume=%.2f\n",
              (unsigned long long)metrics.submitLatency.percentileNs(0.5),
              (unsigned long long)metrics.submitLatency.percentileNs(0.99),
              (unsigned long long)metrics.trades,
              (double)metrics.volumeRaw / 1e8);

  // 4. Reconnect: account 1 had seen through seq K; a resend replays the
  // remainder byte-for-byte with the original per-session seqs.
  const uint64_t total = registry.lastSeq(1);
  CHECK(total > 0);
  const uint64_t k = total / 2;
  CHECK(waitFrames(1, total));  // live delivery flushed through the writer
  const size_t before = frameCount(1);
  CHECK(registry.resendFrom(1, k + 1) == SessionRegistry::ResendResult::Served);
  CHECK(waitFrames(1, before + (total - k)));
  {
    std::lock_guard<std::mutex> lk(wiresMutex);
    const auto& fs1 = wires[1];
    CHECK(fs1.size() == before + (total - k));
    CHECK(SbeOrderEntryCodec::seqOf(fs1[before].data(), fs1[before].size()) == k + 1);
    CHECK(SbeOrderEntryCodec::seqOf(fs1.back().data(), fs1.back().size()) == total);
  }
  for (uint64_t a = 1; a <= 3; ++a)
  {
    registry.detach(a, writers[a - 1]);
    writers[a - 1]->stop();
  }

  // 5. Deterministic recovery: replay the journal (genesis + session) into an
  // EMPTY ledger reproduces the event stream and every balance.
  const auto replayed = Journal::loadTimed(journalPath);
  CHECK(replayed.size() == gen.size() + cmds.size());
  Ledger recovered;
  CHECK(recovered.available(1, BASE) == 0);  // nothing seeded before replay
  CHECK(runReplay(replayed, recovered) == liveHash);

  // 6. Real-money recovery: the recovered ledger balances match the live ledger
  // EXACTLY (per account + venue, base + quote) -- a recovery that reproduced
  // the events but not the balances would be catastrophic.
  for (int a = 1; a <= 3; ++a)
  {
    CHECK(recovered.total(a, BASE) == led.total(a, BASE));
    CHECK(recovered.total(a, QUOTE) == led.total(a, QUOTE));
    CHECK(recovered.available(a, QUOTE) == led.available(a, QUOTE));
    CHECK(recovered.reserved(a, QUOTE) == led.reserved(a, QUOTE));
  }
  CHECK(recovered.total(VENUE, BASE) == led.total(VENUE, BASE));
  CHECK(recovered.total(VENUE, QUOTE) == led.total(VENUE, QUOTE));

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
