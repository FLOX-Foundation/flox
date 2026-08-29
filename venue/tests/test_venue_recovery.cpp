/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * WAL lifecycle and genesis-in-the-WAL:
 * - a shard restart replays its journal instead of truncating it (the O_TRUNC
 *   regression), including after a hard process death (fork + _exit);
 * - the shard journals real, monotonic timestamps and feeds the SAME value to
 *   the engine, so time-dependent behaviour (last-look expiry) replays exactly;
 * - deposits/withdrawals and instrument configuration are sequenced commands,
 *   so a replay from an EMPTY ledger/registry reproduces balances and config.
 */
#include "flox-venue/control_plane.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sequenced_shard.h"

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 900;
constexpr uint64_t kHashSeed = 1469598103934665603ULL;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
int64_t baseRaw(double v) { return static_cast<int64_t>(amountOf(qty(v))); }
int64_t quoteRaw(double v) { return static_cast<int64_t>(amountOf(Volume::fromDouble(v))); }
int64_t i64(Amount a) { return static_cast<int64_t>(a); }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
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

// Deterministic injected clock: base + step, base + 2*step, ...
SequencedShard<>::TimeSource stepClock(int64_t base, int64_t step)
{
  auto t = std::make_shared<int64_t>(base);
  return [t, step]()
  {
    *t += step;
    return *t;
  };
}

struct HashSink : IEngineEventListener
{
  uint64_t h = kHashSeed;
  uint64_t count = 0;
  uint64_t fillHeld = 0;
  uint64_t fillRejected = 0;
  void onEngineEvent(const EngineEventMsg& e) override
  {
    h = hashEvent(h, e.event);
    ++count;
    if (std::get_if<FillHeld>(&e.event))
    {
      ++fillHeld;
    }
    if (std::get_if<FillRejected>(&e.event))
    {
      ++fillRejected;
    }
  }
};

bool ledgersEqual(const Ledger& a, const Ledger& b, int maxAcct)
{
  for (int acct = 1; acct <= maxAcct; ++acct)
  {
    for (AssetId asset : {BASE, QUOTE})
    {
      if (a.available(acct, asset) != b.available(acct, asset) ||
          a.reserved(acct, asset) != b.reserved(acct, asset))
      {
        return false;
      }
    }
  }
  return a.total(VENUE_ACCT, BASE) == b.total(VENUE_ACCT, BASE) &&
         a.total(VENUE_ACCT, QUOTE) == b.total(VENUE_ACCT, QUOTE);
}

// The command stream the crashing child feeds its shard: journaled genesis
// deposits, then crossing flow that leaves resting orders and reservations.
std::vector<InboundCommand> childCommands()
{
  std::vector<InboundCommand> v;
  v.emplace_back(Deposit{1, BASE, baseRaw(1000), SYM});
  v.emplace_back(Deposit{2, QUOTE, quoteRaw(100000), SYM});
  for (uint64_t i = 0; i < 20; ++i)
  {
    v.emplace_back(limit(100 + 2 * i, Side::SELL, 100.0 + static_cast<double>(i % 5) * 0.01, 1.0, 1));
    v.emplace_back(limit(101 + 2 * i, Side::BUY, 100.0 + static_cast<double>(i % 5) * 0.01, 0.5, 2));
  }
  v.emplace_back(CancelOrder{100, SYM, 1});
  v.emplace_back(Withdraw{2, QUOTE, quoteRaw(10), SYM});
  return v;
}

}  // namespace

// A shard dies mid-stream via _exit (no stop(), no flush of the tail); a new
// shard on the same journal must recover the intact prefix and then behave
// exactly like a reference engine that was fed that prefix directly.
TEST(VenueRecovery, ProcessDeathRecoversFromJournal)
{
  const std::string path = "/tmp/flox_test_venue_recovery_procdeath.bin";
  std::remove(path.c_str());

  const auto cmds = childCommands();
  const size_t half = cmds.size() / 2;

  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0)
  {
    // Child: a real shard (journal Sync::Full so every appended record survives
    // the death), killed without any clean shutdown.
    Ledger led;
    auto shard = std::make_unique<SequencedShard<>>(cfg(), path);
    shard->engine().setLedger(&led, VENUE_ACCT);
    HashSink sink;
    shard->subscribeOutbound(&sink);
    shard->start();
    for (size_t i = 0; i < half; ++i)
    {
      shard->submit(cmds[i]);
    }
    shard->flush();  // the first half is guaranteed journaled
    for (size_t i = half; i < cmds.size(); ++i)
    {
      shard->submit(cmds[i]);
    }
    _exit(0);  // hard death: no stop(), consumer possibly mid-record
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));

  // The journal holds an intact prefix: at least the flushed half, never more
  // than what was submitted, with strictly increasing non-zero timestamps.
  const auto records = Journal::loadTimed(path);
  ASSERT_GE(records.size(), half);
  ASSERT_LE(records.size(), cmds.size());
  int64_t prevTs = 0;
  for (const auto& [ts, cmd] : records)
  {
    EXPECT_GT(ts, prevTs);
    prevTs = ts;
  }

  // Reference: a fresh engine + EMPTY ledger fed the journal directly.
  Ledger refLed;
  uint64_t refH = kHashSeed;
  MatchingEngine<MatchingBook> ref(cfg(), [&](const OutboundEvent& e)
                                   { refH = hashEvent(refH, e); });
  ref.setLedger(&refLed, VENUE_ACCT);
  for (const auto& [ts, cmd] : records)
  {
    ref.submit(cmd, ts);
  }

  // Recovered shard: same journal, EMPTY ledger; recovery must run before
  // service and reconstruct the identical book and balances.
  const int64_t tailBase = records.back().first + 1'000'000;
  Ledger recLed;
  HashSink sink;
  auto shard = std::make_unique<SequencedShard<>>(cfg(), path, MatchingBook{},
                                                  Journal::Sync::Full, stepClock(tailBase, 1000));
  shard->engine().setLedger(&recLed, VENUE_ACCT);
  shard->subscribeOutbound(&sink);
  ASSERT_FALSE(shard->ready());
  shard->start();
  ASSERT_TRUE(shard->ready());
  EXPECT_EQ(shard->recoveredCommands(), records.size());
  EXPECT_EQ(sink.count, 0u);  // recovery does not re-broadcast history

  EXPECT_EQ(shard->engine().book().bestBid(), ref.book().bestBid());
  EXPECT_EQ(shard->engine().book().bestAsk(), ref.book().bestAsk());
  EXPECT_TRUE(ledgersEqual(recLed, refLed, 2));

  // Live equivalence: identical post-recovery traffic (same timestamps via the
  // injected clock) must produce an identical event stream on both.
  std::vector<InboundCommand> tail;
  tail.emplace_back(limit(9001, Side::BUY, 100.04, 3.0, 2));
  tail.emplace_back(CancelOrder{9001, SYM, 2});
  tail.emplace_back(limit(9002, Side::SELL, 100.10, 1.0, 1));
  refH = kHashSeed;  // isolate the tail stream on the reference
  for (size_t i = 0; i < tail.size(); ++i)
  {
    shard->submit(tail[i]);
    ref.submit(tail[i], tailBase + static_cast<int64_t>(i + 1) * 1000);
  }
  shard->flush();
  EXPECT_EQ(sink.h, refH);
  EXPECT_EQ(shard->engine().book().bestBid(), ref.book().bestBid());
  EXPECT_EQ(shard->engine().book().bestAsk(), ref.book().bestAsk());
  EXPECT_TRUE(ledgersEqual(recLed, refLed, 2));
  shard->stop();

  // The crashed prefix is still in the file, with the tail appended after it.
  EXPECT_EQ(Journal::loadTimed(path).size(), records.size() + tail.size());
  std::remove(path.c_str());
}

// The core O_TRUNC regression: constructing a shard on an existing journal
// must not erase it -- the restart replays it and keeps appending.
TEST(VenueRecovery, RestartPreservesAndReplaysJournal)
{
  const std::string path = "/tmp/flox_test_venue_recovery_restart.bin";
  std::remove(path.c_str());

  {
    HashSink sink;
    auto shard = std::make_unique<SequencedShard<>>(cfg(), path, MatchingBook{},
                                                    Journal::Sync::Off, stepClock(1'000'000, 1000));
    shard->subscribeOutbound(&sink);
    shard->start();
    EXPECT_EQ(shard->recoveredCommands(), 0u);  // fresh journal
    shard->submit(InboundCommand{limit(1, Side::SELL, 100.00, 5.0, 1)});
    shard->submit(InboundCommand{limit(2, Side::BUY, 99.50, 2.0, 2)});
    shard->submit(InboundCommand{limit(3, Side::SELL, 100.50, 1.0, 1)});
    shard->flush();
    shard->stop();
  }
  const auto before = Journal::loadTimed(path);
  ASSERT_EQ(before.size(), 3u);

  {
    HashSink sink;
    auto shard = std::make_unique<SequencedShard<>>(cfg(), path, MatchingBook{},
                                                    Journal::Sync::Off, stepClock(2'000'000, 1000));
    shard->subscribeOutbound(&sink);
    shard->start();
    EXPECT_EQ(shard->recoveredCommands(), 3u);       // replayed, not wiped
    EXPECT_EQ(Journal::loadTimed(path).size(), 3u);  // file intact after reopen
    EXPECT_EQ(sink.count, 0u);                       // no outbound re-broadcast

    // Recovered book equals a reference engine fed the same journal.
    MatchingEngine<MatchingBook> ref(cfg(), [](const OutboundEvent&) {});
    for (const auto& [ts, cmd] : before)
    {
      ref.submit(cmd, ts);
    }
    EXPECT_EQ(shard->engine().book().bestBid(), ref.book().bestBid());
    EXPECT_EQ(shard->engine().book().bestAsk(), ref.book().bestAsk());

    shard->submit(InboundCommand{CancelOrder{3, SYM, 1}});
    shard->flush();
    shard->stop();
  }
  // Appended after the recovered prefix; nothing truncated at any point.
  const auto after = Journal::loadTimed(path);
  ASSERT_EQ(after.size(), 4u);
  int64_t prevTs = 0;
  for (const auto& [ts, cmd] : after)
  {
    EXPECT_GT(ts, prevTs);  // monotonic across the restart too
    prevTs = ts;
  }
  std::remove(path.c_str());
}

// The shard journals the same timestamp it feeds the engine, so a
// time-dependent scenario (a last-look hold expiring) replays bit-for-bit.
// With the old ts=0 journaling, the hold would never expire on replay.
TEST(VenueRecovery, TimedReplayReproducesLastLookExpiry)
{
  const std::string path = "/tmp/flox_test_venue_recovery_lastlook.bin";
  std::remove(path.c_str());

  venue::SymbolConfig c = cfg();
  c.lastLookWindowNs = DurationNs{5'000};  // five clock steps
  c.lastLookAcceptOnTimeout = false;

  HashSink sink;
  {
    auto shard = std::make_unique<SequencedShard<>>(c, path, MatchingBook{}, Journal::Sync::Off,
                                                    stepClock(1'000'000, 1000));
    shard->subscribeOutbound(&sink);
    shard->start();
    NewOrder maker = limit(1, Side::SELL, 100.00, 5.0, 1);
    maker.lastLook = true;
    shard->submit(InboundCommand{maker});
    shard->submit(InboundCommand{limit(2, Side::BUY, 100.00, 3.0, 2)});  // held fill
    for (OrderId id = 900; id < 906; ++id)
    {
      shard->submit(InboundCommand{CancelOrder{id, SYM, 3}});  // advance time past the window
    }
    shard->flush();
    shard->stop();
  }
  EXPECT_EQ(sink.fillHeld, 1u);
  EXPECT_EQ(sink.fillRejected, 1u);  // the window elapsed live

  const auto records = Journal::loadTimed(path);
  ASSERT_EQ(records.size(), 8u);
  int64_t prevTs = 0;
  for (const auto& [ts, cmd] : records)
  {
    EXPECT_GT(ts, prevTs);  // non-zero, strictly monotonic sequencer time
    prevTs = ts;
  }

  // loadTimed replay reproduces the live stream, including the timed expiry.
  uint64_t recH = kHashSeed;
  uint64_t recRejected = 0;
  MatchingEngine<MatchingBook> rec(c, [&](const OutboundEvent& e)
                                   {
                                     recH = hashEvent(recH, e);
                                     if (std::get_if<FillRejected>(&e))
                                     {
                                       ++recRejected;
                                     } });
  for (const auto& [ts, cmd] : records)
  {
    rec.submit(cmd, ts);
  }
  EXPECT_EQ(recRejected, 1u);
  EXPECT_EQ(recH, sink.h);
  std::remove(path.c_str());
}

// Deposits and withdrawals are journaled commands: replay from an EMPTY ledger
// reproduces every balance, and an uncovered withdraw is rejected (no-op) so
// conservation holds on both runs.
TEST(VenueRecovery, GenesisReplaysFromEmptyLedger)
{
  const std::string path = "/tmp/flox_test_venue_recovery_genesis.bin";
  std::remove(path.c_str());

  const std::vector<std::pair<int64_t, InboundCommand>> cmds{
      {1, InboundCommand{Deposit{1, BASE, baseRaw(10), SYM}}},
      {2, InboundCommand{Deposit{2, QUOTE, quoteRaw(1000), SYM}}},
      {10, InboundCommand{limit(1, Side::SELL, 100, 5, 1)}},
      {20, InboundCommand{limit(2, Side::BUY, 100, 3, 2)}},            // trades 3 @ 100
      {30, InboundCommand{Withdraw{2, QUOTE, quoteRaw(10000), SYM}}},  // > available: rejected
      {40, InboundCommand{Withdraw{1, QUOTE, quoteRaw(100), SYM}}},    // covered: applied
  };

  auto run = [&](Ledger& led)
  {
    uint64_t h = kHashSeed;
    MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                     { h = hashEvent(h, e); });
    eng.setLedger(&led, VENUE_ACCT);
    for (const auto& [ts, cmd] : cmds)
    {
      eng.submit(cmd, ts);
    }
    return h;
  };

  Ledger live;
  {
    Journal j(path);
    for (const auto& [ts, cmd] : cmds)
    {
      j.append(cmd, ts);
    }
    j.flush();
  }
  const uint64_t liveH = run(live);

  // Live semantics: the uncovered withdraw changed nothing; the covered one did.
  EXPECT_EQ(i64(live.available(2, QUOTE)), quoteRaw(700));  // 1000 - 300 spent, withdraw rejected
  EXPECT_EQ(i64(live.available(1, QUOTE)), quoteRaw(200));  // 300 proceeds - 100 withdrawn
  EXPECT_EQ(i64(live.available(2, BASE)), baseRaw(3));

  // Conservation: quote total across accounts + venue = deposited - withdrawn.
  const Amount quoteTotal =
      live.total(1, QUOTE) + live.total(2, QUOTE) + live.total(VENUE_ACCT, QUOTE);
  EXPECT_EQ(i64(quoteTotal), quoteRaw(900));

  // Replay from an EMPTY ledger: no seeding anywhere, balances identical.
  const auto records = Journal::loadTimed(path);
  ASSERT_EQ(records.size(), cmds.size());
  Ledger rec;
  EXPECT_EQ(i64(rec.available(1, BASE)), 0);
  const uint64_t recH = run(rec);
  EXPECT_EQ(recH, liveH);
  EXPECT_TRUE(ledgersEqual(rec, live, 2));
  std::remove(path.c_str());
}

// Instrument configuration is journaled: replaying ListInstrument + SetBands +
// SetTriggerRef + AdminCmd(Halt) into a fresh registry and a fresh engine
// reproduces the listed instrument, its bands, its trigger reference, and the
// halt.
TEST(VenueRecovery, ConfigReplayReproducesInstrumentState)
{
  const std::string path = "/tmp/flox_test_venue_recovery_config.bin";
  std::remove(path.c_str());

  const std::vector<std::pair<int64_t, InboundCommand>> cmds{
      {10, InboundCommand{ListInstrument{SYM, px(0.01), Quantity{}, px(50), px(150)}}},
      {20, InboundCommand{SetBands{SYM, px(90), px(110)}}},
      {25, InboundCommand{SetTriggerRef{SYM, TriggerRef::Mark}}},
      {30, InboundCommand{AdminCmd{SYM, AdminAction::Halt}}},
  };
  {
    Journal j(path);
    for (const auto& [ts, cmd] : cmds)
    {
      j.append(cmd, ts);
    }
    j.flush();
  }

  const auto records = Journal::loadTimed(path);
  ASSERT_EQ(records.size(), cmds.size());

  // Registry side: the WAL rebuilds the configuration store.
  InstrumentRegistry reg;
  for (const auto& [ts, cmd] : records)
  {
    EXPECT_TRUE(reg.apply(cmd));
  }
  ASSERT_TRUE(reg.has(SYM));
  EXPECT_EQ(reg.get(SYM)->tickSize, px(0.01));
  EXPECT_EQ(reg.get(SYM)->minPrice, px(90));
  EXPECT_EQ(reg.get(SYM)->maxPrice, px(110));
  EXPECT_EQ(reg.get(SYM)->triggerRef, TriggerRef::Mark);
  EXPECT_TRUE(reg.get(SYM)->halted);

  // Engine side: the same stream reproduces the tightened band and the halt.
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> fresh(cfg(), [&](const OutboundEvent& e)
                                     { ev.push_back(e); });
  for (const auto& [ts, cmd] : records)
  {
    fresh.submit(cmd, ts);
  }
  EXPECT_EQ(fresh.config().minPrice, px(90));
  EXPECT_EQ(fresh.config().maxPrice, px(110));
  EXPECT_EQ(fresh.config().triggerRef, TriggerRef::Mark);
  EXPECT_TRUE(fresh.config().halted);
  ev.clear();
  fresh.submit(InboundCommand{limit(1, Side::SELL, 100, 1, 1)}, 40);
  bool rejectedHalted = false;
  for (const auto& e : ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e); r && r->reason == RejectReason::Halted)
    {
      rejectedHalted = true;
    }
  }
  EXPECT_TRUE(rejectedHalted);
  std::remove(path.c_str());
}
