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
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/ouch_codec.h"
#include "flox-venue/resend_buffer.h"
#include "flox/book/matching_book.h"

#include "flox/backtest/fee_schedule.h"

#include <gtest/gtest.h>
#include <chrono>

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

uint64_t runReplay(const std::vector<std::pair<int64_t, InboundCommand>>& cmds, Ledger& led)
{
  for (int a = 1; a <= 3; ++a)
  {
    led.deposit(a, BASE, base(1000));
    led.deposit(a, QUOTE, quote(100000));
  }
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
  std::vector<std::pair<int64_t, InboundCommand>> cmds;
  cmds.emplace_back(10, InboundCommand{AdminCmd{SYM, AdminAction::BeginPreOpen}});
  cmds.emplace_back(20, InboundCommand{limit(1, Side::SELL, 100, 5, 1)});             // accumulate (no match)
  cmds.emplace_back(30, InboundCommand{limit(2, Side::BUY, 100, 5, 2)});              // crossed book in pre-open
  cmds.emplace_back(40, InboundCommand{AdminCmd{SYM, AdminAction::OpenContinuous}});  // uncross @100

  // Live run: journal every command, hash the event stream, count trades.
  Ledger liveLed;
  for (int a = 1; a <= 3; ++a)
  {
    liveLed.deposit(a, BASE, base(1000));
    liveLed.deposit(a, QUOTE, quote(100000));
  }
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

  // Recovery: replay the journal into a fresh engine; state must match bit-for-bit.
  const auto replayed = Journal::loadTimed(jpath);
  CHECK(replayed.size() == cmds.size());  // AdminCmds round-tripped (not truncated at an unknown tag)
  Ledger recLed;
  for (int a = 1; a <= 3; ++a)
  {
    recLed.deposit(a, BASE, base(1000));
    recLed.deposit(a, QUOTE, quote(100000));
  }
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
  for (int a = 1; a <= 3; ++a)
  {
    led.deposit(a, BASE, base(1000));
    led.deposit(a, QUOTE, quote(100000));
  }
  const Amount initBase = base(1000) * 3;
  const Amount initQuote = quote(100000) * 3;

  Metrics metrics;
  ResendBuffer resend;
  MarketDataPublisher<> md([](const MdMessage&) {}, px(0.01), SYM);
  const std::string journalPath = "/tmp/flox_test_venue_venue_venue_journal.bin";
  Journal journal(journalPath);
  const uint64_t clientSession = 7;
  uint64_t liveHash = 1469598103934665603ULL;

  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     metrics.observe(e);
                                     md.onEvent(e);
                                     resend.append(clientSession, e, /*ts*/ 0);
                                     liveHash = hashEvent(liveHash, e); });
  flox::FeeSchedule fs;
  fs.addTier(0.0, -1.0, 2.0);
  eng.setFeeSchedule(fs);
  eng.setLedger(&led, VENUE);

  // Drive the session through the OUCH wire codec, timing each submit.
  const auto cmds = session();
  for (const auto& [ts, cmd] : cmds)
  {
    std::vector<uint8_t> wire;
    OuchCodec::encode(cmd, wire);
    auto decoded = OuchCodec::decode(wire.data(), wire.size());
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

  // 4. Reconnect: client had seen through seq K, resends the remainder.
  const uint64_t total = resend.lastSeq(clientSession);
  CHECK(total > 0);
  const uint64_t k = total / 2;
  const auto gap = resend.resend(clientSession, k + 1);
  CHECK(gap.size() == total - k);
  CHECK(gap.front().seq == k + 1 && gap.back().seq == total);

  // 5. Deterministic recovery: replay the journal reproduces the event stream.
  const auto replayed = Journal::loadTimed(journalPath);
  CHECK(replayed.size() == cmds.size());
  Ledger recovered;
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
