// The one checkpoint behaviour that reaches the perimeter: the onCheckpoint
// hook persisting the FIX session sidecar. It lives here rather than beside
// the other checkpoint tests so that those nineteen -- crash recovery among
// them -- keep running on a platform the perimeter does not reach.
/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Checkpoint + journal rotation. The snapshot is a journal-format file whose
 * records rebuild engine state through the same apply paths live traffic uses;
 * these tests pin the property that makes that safe: recovery from
 * snapshot+tail is indistinguishable from a full-history replay -- state hash
 * equal AND the event stream of identical subsequent traffic equal -- with
 * open last-look holds, pending stops, GTD, OCO, icebergs, pegs, perp
 * positions and reservations all restored, torn snapshots falling back a
 * generation, retention pruning old generations, and snapshot-only records
 * rejected from live traffic.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sequenced_shard.h"
#include "flox-venue/session_registry.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 900;
constexpr int NACCT = 6;
constexpr uint64_t kHashSeed = 1469598103934665603ULL;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
int64_t baseRaw(double v) { return static_cast<int64_t>(amountOf(qty(v))); }
int64_t quoteRaw(double v) { return static_cast<int64_t>(amountOf(Volume::fromDouble(v))); }

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

// Settable deterministic clock: strictly advancing by `step` per call, with a
// jumpable base (post-restart GTD expiry needs a controlled time jump).
using ClockState = std::shared_ptr<std::atomic<int64_t>>;
ClockState clockState(int64_t base) { return std::make_shared<std::atomic<int64_t>>(base); }
SequencedShard<>::TimeSource clockOf(ClockState t, int64_t step = 1000)
{
  return [t, step]()
  { return t->fetch_add(step) + step; };
}

struct HashSink : IEngineEventListener
{
  uint64_t h = kHashSeed;
  uint64_t count = 0;
  uint64_t fillHeld = 0;
  std::vector<OutboundEvent> events;
  void onEngineEvent(const EngineEventMsg& e) override
  {
    h = hashEvent(h, e.event);
    ++count;
    if (std::get_if<FillHeld>(&e.event))
    {
      ++fillHeld;
    }
    events.push_back(e.event);
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

// Every path built from this carries the PID: two invocations of this binary
// racing on the same snapshot/journal files (a manual rerun alongside
// `ctest -j`, or any future gtest sharding that launches the same executable
// twice) corrupt each other's generations mid-test rather than failing
// cleanly, which is what made this suite flaky under parallel/contended runs.
std::string pidPath(const std::string& suffix)
{
  return tmpPath("venue_" + suffix);
}

// Remove the legacy file and every checkpoint generation of `base`.
void cleanFiles(const std::string& base)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove(base, ec);
  const auto g = SequencedShard<>::scanGenerations(base);
  for (int64_t ts : g.snapshots)
  {
    fs::remove(SequencedShard<>::snapshotPath(base, ts), ec);
    fs::remove(SequencedShard<>::snapshotPath(base, ts) + ".tmp", ec);
  }
  for (int64_t ts : g.segments)
  {
    fs::remove(SequencedShard<>::segmentPath(base, ts), ec);
  }
}

// Full retained history: legacy file (if present) followed by every journal
// segment in checkpoint order -- the "full replay" reference stream.
std::vector<std::pair<int64_t, InboundCommand>> allRecords(const std::string& base)
{
  auto all = Journal::loadTimed(base);
  for (int64_t ts : SequencedShard<>::scanGenerations(base).segments)
  {
    const auto seg = Journal::loadTimed(SequencedShard<>::segmentPath(base, ts));
    all.insert(all.end(), seg.begin(), seg.end());
  }
  return all;
}

struct Rng
{
  uint64_t s;
  uint64_t next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

// Random command mix covering the whole surface the checkpoint must carry:
// limits (lastLook / iceberg / GTD / OCO / peg / IOC / FOK / postOnly / STP),
// markets, stops (market / limit / trailing), cancels, modifies, quotes,
// last-look decisions, time ticks and (optionally) deposits/withdrawals.
InboundCommand randomCmd(Rng& rng, OrderId& nextId, int i, bool moneyFlow)
{
  const uint64_t r = rng.next();
  const uint32_t kind = r % 100;
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  const uint64_t acct = 1 + (r >> 8) % NACCT;

  if (kind < 10 && nextId > 1)
  {
    return InboundCommand{CancelOrder{1 + (r >> 16) % (nextId - 1), SYM, 0}};
  }
  if (kind < 16 && nextId > 1)
  {
    const OrderId vid = 1 + (r >> 16) % (nextId - 1);
    const int ticks = static_cast<int>((r >> 32) % 81) - 40;
    const Price np = ((r >> 40) % 4 == 0)
                         ? Price{}  // keep current price
                         : Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
    return InboundCommand{ModifyOrder{vid, SYM, np, qty(1.0 + static_cast<double>((r >> 44) % 4)), 0}};
  }
  if (kind < 20)
  {
    // Random held id: mostly stale/wrong (deterministic rejects), sometimes live.
    return InboundCommand{LastLookDecision{1 + (r >> 16) % 64, SYM, (r & 8) != 0, acct}};
  }
  if (kind < 23)
  {
    const OrderId bidId = nextId;
    const OrderId askId = nextId + 1;
    nextId += 2;
    const int bt = static_cast<int>((r >> 32) % 30);
    return InboundCommand{Quote{bidId, askId, SYM, Price::fromRaw(midRaw - (1 + bt) * tickRaw),
                                qty(1.0 + static_cast<double>((r >> 40) % 3)),
                                Price::fromRaw(midRaw + (1 + bt) * tickRaw),
                                qty(1.0 + static_cast<double>((r >> 44) % 3)), acct}};
  }
  if (kind < 25 && moneyFlow)
  {
    return (r & 4) != 0
               ? InboundCommand{Deposit{acct, QUOTE, quoteRaw(500.0), SYM}}
               : InboundCommand{Deposit{acct, BASE, baseRaw(5.0), SYM}};
  }
  if (kind < 27 && moneyFlow)
  {
    return (r & 4) != 0 ? InboundCommand{Withdraw{acct, QUOTE, quoteRaw(200.0), SYM}}
                        : InboundCommand{Withdraw{acct, BASE, baseRaw(2.0), SYM}};
  }
  if (kind < 29)
  {
    return InboundCommand{TimeTick{SYM}};
  }

  NewOrder o;
  o.id = nextId++;
  o.symbol = SYM;
  o.side = (r & 1) ? Side::BUY : Side::SELL;
  o.accountId = acct;
  const int ticks = static_cast<int>((r >> 1) % 101) - 50;
  o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
  o.quantity = qty(1.0 + static_cast<double>((r >> 20) % 5));
  o.type = (kind < 34) ? OrderType::MARKET : OrderType::LIMIT;
  if (kind >= 34 && kind < 40)  // conditional flavors
  {
    const uint32_t which = static_cast<uint32_t>((r >> 36) % 4);
    const int tt = static_cast<int>((r >> 33) % 61) - 30;
    o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(tt) * tickRaw);
    if (which == 0)
    {
      o.type = OrderType::STOP_MARKET;
    }
    else if (which == 1)
    {
      o.type = OrderType::STOP_LIMIT;
    }
    else if (which == 2)
    {
      o.type = OrderType::TAKE_PROFIT_MARKET;
    }
    else
    {
      o.type = OrderType::TRAILING_STOP;
      o.trailingOffset = Price::fromRaw((1 + static_cast<int64_t>((r >> 48) % 50)) * tickRaw);
    }
  }
  else if (o.type == OrderType::LIMIT)
  {
    const uint32_t t = static_cast<uint32_t>((r >> 32) % 100);
    if (t < 25)
    {
      o.lastLook = true;
    }
    else if (t < 35)
    {
      o.visibleQuantity = qty(1.0);  // iceberg peak
    }
    else if (t < 45)
    {
      o.tif = TimeInForce::GTD;
      o.expiryNs = SeqNanos::fromRaw(1'000'000 + static_cast<int64_t>(i) * 1000 +
                                     (1 + static_cast<int64_t>((r >> 40) % 60)) * 1000);
    }
    else if (t < 53)
    {
      o.ocoGroup = 1 + ((r >> 44) % 5);
    }
    else if (t < 60)
    {
      const uint32_t pr = static_cast<uint32_t>((r >> 48) % 3);
      o.peg = pr == 0 ? PegRef::Bid : (pr == 1 ? PegRef::Ask : PegRef::Mid);
      o.pegOffsetRaw = (static_cast<int64_t>((r >> 52) % 5) - 2) * tickRaw;
    }
    else if (t < 68)
    {
      o.tif = TimeInForce::IOC;
    }
    else if (t < 74)
    {
      o.tif = TimeInForce::FOK;
    }
    else if (t < 80)
    {
      o.postOnly = true;
    }
    else if (t < 85)
    {
      o.stp = STPMode::CancelOldest;
    }
    else if (t < 90)
    {
      o.clientOrderId = 1 + (r >> 52) % 500;  // dedup set gets real load
    }
  }
  return InboundCommand{o};
}

}  // namespace

// Engine-level round trip: writeSnapshot -> applySnapshotRecord into a fresh
// engine + empty ledger reproduces the exact state (hash, book, balances) --
// no shard machinery involved.
// BalanceUpdate across checkpoint/recovery: a live deposit emits exactly one
// BalanceUpdate; recovery (snapshot Deposit records + journal tail) publishes
// NOTHING outbound -- reconnecting clients reconcile via snapshots, not a
// re-broadcast; a fresh post-recovery deposit emits exactly one again. The
// onCheckpoint hook fires at the checkpoint boundary and the FIX session
// sidecar written there restores into a fresh host + registry.
TEST(VenueCheckpoint, BalanceUpdateRecoverySuppressionAndSidecarHook)
{
  const std::string base = pidPath("checkpoint_balance") + ".bin";
  const std::string sidecar = FixSessionSidecar::pathFor(base);
  cleanFiles(base);
  std::remove(sidecar.c_str());

  const auto countBalance = [](const std::vector<OutboundEvent>& events)
  {
    size_t n = 0;
    for (const auto& e : events)
    {
      n += std::get_if<BalanceUpdate>(&e) != nullptr ? 1 : 0;
    }
    return n;
  };

  Ledger led1;
  HashSink sink1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->subscribeOutbound(&sink1);

  // FIX session state as the gateway harness would hold it at runtime.
  FixSessionHost host;
  SessionRegistry registry;
  {
    auto st = host.stateOf(7);
    std::lock_guard<std::mutex> lk(st->m);
    st->expectedIn = 42;
    st->established = true;
  }
  int64_t hookTs = 0;
  s1->onCheckpoint([&](int64_t ts)
                   {
                     hookTs = ts;
                     ASSERT_TRUE(FixSessionSidecar::write(sidecar, host, registry)); });
  s1->start();

  s1->submit(InboundCommand{Deposit{1, QUOTE, quoteRaw(100), SYM}});
  s1->flush();
  EXPECT_EQ(countBalance(sink1.events), 1u);  // the live deposit reported once

  ASSERT_TRUE(s1->checkpointNow());
  EXPECT_GT(hookTs, 0);                                               // the hook rode the checkpoint boundary
  s1->submit(InboundCommand{Withdraw{1, QUOTE, quoteRaw(30), SYM}});  // journal-tail record
  s1->flush();
  EXPECT_EQ(countBalance(sink1.events), 2u);
  s1->stop();
  s1.reset();

  // The sidecar restores into a fresh host + registry (restart semantics are
  // pinned end-to-end in test_venue_fix_session).
  FixSessionHost host2;
  SessionRegistry registry2;
  ASSERT_TRUE(FixSessionSidecar::load(sidecar, host2, registry2));
  {
    auto st = host2.stateOf(7);
    std::lock_guard<std::mutex> lk(st->m);
    EXPECT_EQ(st->expectedIn, 42u);
    EXPECT_TRUE(st->established);
  }

  // Recovery: snapshot RestoreBalance records AND the journal-tail Withdraw
  // replay into the engine without a single outbound event.
  Ledger led2;
  HashSink sink2;
  auto t2 = clockState(t1->load() + 1'000'000);
  auto s2 = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->engine().setLedger(&led2, VENUE_ACCT);
  s2->subscribeOutbound(&sink2);
  s2->start();
  EXPECT_GT(s2->recoveredCommands(), 0u);
  EXPECT_EQ(sink2.count, 0u);  // no re-broadcast of recovered history at all
  EXPECT_EQ(led2.available(1, QUOTE), led1.available(1, QUOTE));

  // A fresh deposit after recovery reports exactly once, as live.
  s2->submit(InboundCommand{Deposit{1, QUOTE, quoteRaw(5), SYM}});
  s2->flush();
  EXPECT_EQ(countBalance(sink2.events), 1u);
  s2->stop();
  cleanFiles(base);
  std::remove(sidecar.c_str());
}

// Exact balances: a moment the old Deposit-total encoding could not represent
// (negative available mid-liquidation) now snapshots and restores bit-for-bit
// via RestoreBalance -- no generation fallback, no hash mismatch.
