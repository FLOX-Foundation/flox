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

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
      o.expiryNs = 1'000'000 + static_cast<int64_t>(i) * 1000 +
                   (1 + static_cast<int64_t>((r >> 40) % 60)) * 1000;
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
TEST(VenueCheckpoint, EngineSnapshotRoundTrip)
{
  const std::string path = "/tmp/flox_test_venue_checkpoint_roundtrip.snap";
  std::remove(path.c_str());

  venue::SymbolConfig c = cfg();
  c.lastLookWindowNs = 10'000'000;

  Ledger led;
  MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  eng.submit(InboundCommand{Deposit{1, BASE, baseRaw(100), SYM}}, 1000);
  eng.submit(InboundCommand{Deposit{2, QUOTE, quoteRaw(100000), SYM}}, 2000);
  NewOrder maker = limit(1, Side::SELL, 100.00, 5.0, 1);
  maker.lastLook = true;
  eng.submit(InboundCommand{maker}, 3000);
  NewOrder ice = limit(2, Side::SELL, 101.00, 10.0, 1);
  ice.visibleQuantity = qty(2.0);
  eng.submit(InboundCommand{ice}, 4000);
  NewOrder taker = limit(3, Side::BUY, 100.00, 3.0, 2);
  taker.tif = TimeInForce::IOC;
  eng.submit(InboundCommand{taker}, 5000);  // held fill stays open
  NewOrder stop = limit(4, Side::SELL, 0.0, 2.0, 1);
  stop.type = OrderType::STOP_MARKET;
  stop.triggerPrice = px(95.0);
  eng.submit(InboundCommand{stop}, 6000);
  NewOrder gtd = limit(5, Side::BUY, 99.0, 2.0, 2);
  gtd.tif = TimeInForce::GTD;
  gtd.expiryNs = 50'000'000;
  eng.submit(InboundCommand{gtd}, 7000);

  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    eng.writeSnapshot(out);
    out.flush();
  }

  Ledger led2;
  MatchingEngine<MatchingBook> rec(c, [](const OutboundEvent&) {});
  rec.setLedger(&led2, VENUE_ACCT);
  const auto records = Journal::loadTimed(path);
  ASSERT_GE(records.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<SnapshotBegin>(records.front().second));
  EXPECT_TRUE(std::holds_alternative<SnapshotEnd>(records.back().second));
  for (const auto& [ts, cmd] : records)
  {
    ASSERT_TRUE(rec.applySnapshotRecord(cmd, ts));
  }

  EXPECT_EQ(rec.stateHash(), eng.stateHash());
  EXPECT_EQ(rec.book().bestBid(), eng.book().bestBid());
  EXPECT_EQ(rec.book().bestAsk(), eng.book().bestAsk());
  EXPECT_TRUE(ledgersEqual(led2, led, 2));
  ASSERT_NE(rec.book().find(2), nullptr);
  EXPECT_EQ(rec.book().find(2)->hidden, qty(8.0));
  EXPECT_EQ(rec.book().find(2)->leaves, qty(2.0));
  std::remove(path.c_str());
}

// The core differential guarantee: a long random session with a checkpoint at
// an arbitrary point recovers (snapshot + tail segments) into EXACTLY the
// state a full-history replay produces -- state hash equal AND the event hash
// of identical subsequent traffic equal.
TEST(VenueCheckpoint, DifferentialRandomSessionCheckpointInvisible)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_diff.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  c.lastLookWindowNs = 20'000;  // 20 clock steps: holds open and expire mid-stream
  c.lastLookAcceptOnTimeout = false;

  Ledger led1;
  HashSink sink1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->subscribeOutbound(&sink1);
  s1->start();
  for (int a = 1; a <= NACCT; ++a)
  {
    s1->submit(InboundCommand{Deposit{static_cast<uint64_t>(a), BASE, baseRaw(1000), SYM}});
    s1->submit(InboundCommand{Deposit{static_cast<uint64_t>(a), QUOTE, quoteRaw(100000), SYM}});
  }

  Rng rng{0xC0FFEE0DDBA11ULL};
  OrderId nextId = 1;
  const int N = 4000;
  const int checkpointAt = 2400;
  for (int i = 0; i < N; ++i)
  {
    if (i == checkpointAt)
    {
      // Force checkpoint-relevant state RIGHT before the snapshot: an open
      // hold, a pending stop and an iceberg must be in the file.
      NewOrder mk = limit(nextId++, Side::SELL, 100.00, 4.0, 1);
      mk.lastLook = true;
      s1->submit(InboundCommand{mk});
      NewOrder tk = limit(nextId++, Side::BUY, 100.00, 2.0, 2);
      tk.tif = TimeInForce::IOC;
      s1->submit(InboundCommand{tk});
      NewOrder st = limit(nextId++, Side::SELL, 0.0, 1.0, 3);
      st.type = OrderType::STOP_MARKET;
      st.triggerPrice = px(60.0);
      s1->submit(InboundCommand{st});
      NewOrder ic = limit(nextId++, Side::BUY, 51.0, 6.0, 4);
      ic.visibleQuantity = qty(1.0);
      s1->submit(InboundCommand{ic});
      ASSERT_TRUE(s1->checkpointNow());
    }
    s1->submit(randomCmd(rng, nextId, i, /*moneyFlow*/ true));
  }
  s1->flush();
  EXPECT_GT(sink1.fillHeld, 20u);  // coverage: holds really happened
  const uint64_t liveHash = s1->engine().stateHash();
  s1->stop();
  s1.reset();

  // Anti-vacuum guard: the snapshot FILE itself must carry the hard state --
  // a checkpoint that silently serialized an earlier/emptier boundary (or
  // nothing) must fail here, not be papered over by the tail replay below.
  const auto gens = SequencedShard<>::scanGenerations(base);
  ASSERT_EQ(gens.snapshots.size(), 1u);
  ASSERT_EQ(gens.segments.size(), 1u);
  const auto snapRecords =
      Journal::loadTimed(SequencedShard<>::snapshotPath(base, gens.snapshots[0]));
  size_t nHeld = 0, nStops = 0, nIceberg = 0, nOrders = 0, nBalances = 0;
  for (const auto& [ts, cmd] : snapRecords)
  {
    if (std::get_if<RestoreHeld>(&cmd))
    {
      ++nHeld;
    }
    if (std::get_if<RestoreStop>(&cmd))
    {
      ++nStops;
    }
    if (std::get_if<RestoreBalance>(&cmd))
    {
      ++nBalances;
    }
    if (const auto* r = std::get_if<RestoreOrder>(&cmd))
    {
      ++nOrders;
      nIceberg += r->hidden.raw() > 0 ? 1 : 0;
    }
  }
  EXPECT_GE(nHeld, 1u);
  EXPECT_GE(nStops, 1u);
  EXPECT_GE(nIceberg, 1u);
  EXPECT_GT(nOrders, 2u);
  EXPECT_GE(nBalances, static_cast<size_t>(NACCT));  // every funded account x asset

  // Reference: full-history replay (legacy + all segments) from empty.
  const auto all = allRecords(base);
  ASSERT_FALSE(all.empty());
  Ledger refLed;
  uint64_t refH = kHashSeed;
  MatchingEngine<MatchingBook> ref(c, [&](const OutboundEvent& e)
                                   { refH = hashEvent(refH, e); });
  ref.setLedger(&refLed, VENUE_ACCT);
  for (const auto& [ts, cmd] : all)
  {
    ref.submit(cmd, ts);
  }
  EXPECT_EQ(ref.stateHash(), liveHash);  // the reference really reproduces live

  // Recovered shard: snapshot + tail. State hash AND ledger split must match.
  Ledger led2;
  HashSink sink2;
  const int64_t tailBase = all.back().first + 1'000'000;
  auto t2 = clockState(tailBase);
  auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->engine().setLedger(&led2, VENUE_ACCT);
  s2->subscribeOutbound(&sink2);
  s2->start();
  // Recovery PROVABLY consumed the snapshot, not a full-history replay: it
  // applied exactly the snapshot's records plus the tail segment's.
  const size_t tailRecords =
      Journal::loadTimed(SequencedShard<>::segmentPath(base, gens.segments[0])).size();
  EXPECT_EQ(s2->recoveredFromSnapshotRecords(), snapRecords.size());
  EXPECT_EQ(s2->recoveredCommands(), snapRecords.size() + tailRecords);
  EXPECT_LT(s2->recoveredCommands(), all.size());  // strictly fewer than full history
  EXPECT_EQ(s2->engine().stateHash(), liveHash);
  EXPECT_EQ(s2->engine().stateHash(), ref.stateHash());
  EXPECT_TRUE(ledgersEqual(led2, refLed, NACCT));

  // Reserved invariant: available+reserved per account x asset unchanged.
  for (int a = 1; a <= NACCT; ++a)
  {
    for (AssetId asset : {BASE, QUOTE})
    {
      EXPECT_EQ(led2.available(a, asset) + led2.reserved(a, asset),
                refLed.available(a, asset) + refLed.reserved(a, asset));
    }
  }

  // Identical subsequent traffic must produce an identical event stream.
  refH = kHashSeed;
  sink2.h = kHashSeed;
  std::vector<InboundCommand> tail;
  Rng rng2{0xFACEFEEDULL};
  for (int i = 0; i < 200; ++i)
  {
    tail.push_back(randomCmd(rng2, nextId, N + i, true));
  }
  for (size_t i = 0; i < tail.size(); ++i)
  {
    s2->submit(tail[i]);
    ref.submit(tail[i], tailBase + static_cast<int64_t>(i + 1) * 1000);
  }
  s2->flush();
  EXPECT_EQ(sink2.h, refH);
  EXPECT_EQ(s2->engine().stateHash(), ref.stateHash());
  EXPECT_TRUE(ledgersEqual(led2, refLed, NACCT));
  s2->stop();
  cleanFiles(base);
}

// Addressable restore checks: open hold (acceptable post-restart, settles),
// pending stop + trailing stop, GTD (expires on the restored deadline), OCO
// across book and stop book, iceberg (hidden reserve intact), peg (keeps
// repricing), clientOrderId dedup, and the reserved invariant.
TEST(VenueCheckpoint, CheckpointRestoresOpenStateAddressably)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_state.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  c.lastLookWindowNs = 100'000'000;  // survives the restart window
  c.lastLookAcceptOnTimeout = false;

  Ledger led1;
  HashSink sink1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->engine().setMmp(1, qty(100.0), 1'000'000'000);
  s1->subscribeOutbound(&sink1);
  s1->start();

  s1->submit(InboundCommand{Deposit{1, BASE, baseRaw(1000), SYM}});
  s1->submit(InboundCommand{Deposit{2, QUOTE, quoteRaw(100000), SYM}});
  s1->submit(InboundCommand{Deposit{3, BASE, baseRaw(1000), SYM}});
  s1->submit(InboundCommand{Deposit{3, QUOTE, quoteRaw(100000), SYM}});
  s1->submit(InboundCommand{Deposit{4, QUOTE, quoteRaw(100000), SYM}});

  NewOrder mk = limit(1, Side::SELL, 100.00, 5.0, 1);
  mk.lastLook = true;
  s1->submit(InboundCommand{mk});
  NewOrder tk = limit(2, Side::BUY, 100.00, 3.0, 2);
  tk.tif = TimeInForce::IOC;
  s1->submit(InboundCommand{tk});  // hold #1 (qty 3) stays open
  NewOrder ice = limit(3, Side::SELL, 101.00, 10.0, 1);
  ice.visibleQuantity = qty(2.0);
  s1->submit(InboundCommand{ice});
  NewOrder gtd = limit(4, Side::BUY, 99.00, 2.0, 2);
  gtd.tif = TimeInForce::GTD;
  gtd.expiryNs = 5'000'000;
  s1->submit(InboundCommand{gtd});
  NewOrder ocoBook = limit(5, Side::BUY, 98.00, 2.0, 2);
  ocoBook.ocoGroup = 7;
  s1->submit(InboundCommand{ocoBook});
  NewOrder ocoStop = limit(6, Side::SELL, 0.0, 2.0, 3);
  ocoStop.type = OrderType::STOP_MARKET;
  ocoStop.triggerPrice = px(90.0);
  ocoStop.ocoGroup = 7;
  s1->submit(InboundCommand{ocoStop});
  NewOrder trail = limit(7, Side::SELL, 0.0, 1.0, 3);
  trail.type = OrderType::TRAILING_STOP;
  trail.trailingOffset = px(1.00);
  s1->submit(InboundCommand{trail});
  NewOrder peg = limit(8, Side::BUY, 0.0, 1.0, 4);
  peg.peg = PegRef::Bid;
  s1->submit(InboundCommand{peg});
  NewOrder clord = limit(9, Side::BUY, 97.00, 1.0, 4);
  clord.clientOrderId = 555;
  s1->submit(InboundCommand{clord});

  ASSERT_TRUE(s1->checkpointNow());
  s1->flush();
  const uint64_t liveHash = s1->engine().stateHash();
  s1->stop();
  s1.reset();

  Ledger led2;
  HashSink sink2;
  auto t2 = clockState(1'200'000);
  auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->engine().setLedger(&led2, VENUE_ACCT);
  s2->subscribeOutbound(&sink2);
  s2->start();

  // Full state equality first (covers MMP config, pegs, counters, ...), and
  // recovery provably came through the snapshot.
  EXPECT_GT(s2->recoveredFromSnapshotRecords(), 0u);
  EXPECT_EQ(s2->engine().stateHash(), liveHash);
  EXPECT_TRUE(ledgersEqual(led2, led1, 4));

  // Iceberg: displayed peak and hidden reserve intact.
  const RestingOrder* iceR = s2->engine().book().find(3);
  ASSERT_NE(iceR, nullptr);
  EXPECT_EQ(iceR->leaves, qty(2.0));
  EXPECT_EQ(iceR->hidden, qty(8.0));
  EXPECT_EQ(iceR->peak, qty(2.0));

  // Maker with the open hold: displayed remainder resting, lastLook intact.
  const RestingOrder* mkR = s2->engine().book().find(1);
  ASSERT_NE(mkR, nullptr);
  EXPECT_EQ(mkR->leaves, qty(2.0));
  EXPECT_TRUE(mkR->lastLook);

  // Pending stops (incl. trailing) visible in the account snapshot.
  const auto acct3 = s2->engine().snapshotAccount(3);
  ASSERT_EQ(acct3.pendingStops.size(), 2u);

  // The restored hold accepts and SETTLES: trade at 100 x 3, money moves.
  const Amount a1qBefore = led2.available(1, QUOTE);
  const Amount a2bBefore = led2.available(2, BASE);
  s2->submit(InboundCommand{LastLookDecision{1, SYM, true, 1}});
  s2->flush();
  bool sawTrade = false;
  for (const auto& e : sink2.events)
  {
    if (const auto* t = std::get_if<venue::Trade>(&e);
        t && t->makerId == 1 && t->takerId == 2 && t->quantity == qty(3.0) && t->price == px(100.0))
    {
      sawTrade = true;
    }
  }
  EXPECT_TRUE(sawTrade);
  EXPECT_EQ(led2.available(1, QUOTE) - a1qBefore, quoteRaw(300.0));
  EXPECT_EQ(led2.available(2, BASE) - a2bBefore, baseRaw(3.0));

  // clientOrderId dedup survived: same clOrdId rejects post-restart.
  NewOrder dup = limit(20, Side::BUY, 97.50, 1.0, 4);
  dup.clientOrderId = 555;
  s2->submit(InboundCommand{dup});
  s2->flush();
  bool sawDupReject = false;
  for (const auto& e : sink2.events)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e);
        r && r->id == 20 && r->reason == RejectReason::DuplicateClientOrderId)
    {
      sawDupReject = true;
    }
  }
  EXPECT_TRUE(sawDupReject);

  // GTD: jump time past the restored expiry -> canceled Expired.
  t2->store(6'000'000);
  s2->submit(InboundCommand{TimeTick{SYM}});
  s2->flush();
  bool sawExpiry = false;
  for (const auto& e : sink2.events)
  {
    if (const auto* x = std::get_if<OrderCanceled>(&e);
        x && x->id == 4 && x->reason == CancelReason::Expired)
    {
      sawExpiry = true;
    }
  }
  EXPECT_TRUE(sawExpiry);

  // OCO across book and stop book: filling the book leg cancels the stop leg.
  s2->submit(InboundCommand{limit(21, Side::SELL, 98.00, 2.0, 3)});
  s2->flush();
  bool sawOco = false;
  for (const auto& e : sink2.events)
  {
    if (const auto* x = std::get_if<OrderCanceled>(&e);
        x && x->id == 6 && x->reason == CancelReason::OcoTriggered)
    {
      sawOco = true;
    }
  }
  EXPECT_TRUE(sawOco);

  // Peg: still live and repricing (an OrderModified for it appeared after the
  // book moved under it).
  EXPECT_TRUE(s2->engine().book().contains(8));
  bool sawRepeg = false;
  for (const auto& e : sink2.events)
  {
    if (const auto* m = std::get_if<OrderModified>(&e); m && m->id == 8)
    {
      sawRepeg = true;
    }
  }
  EXPECT_TRUE(sawRepeg);

  // Drain: every reservation returns to available.
  for (OrderId id = 1; id <= 21; ++id)
  {
    s2->submit(InboundCommand{CancelOrder{id, SYM, 0}});
  }
  s2->flush();
  for (int a = 1; a <= 4; ++a)
  {
    EXPECT_EQ(led2.reserved(a, BASE), 0);
    EXPECT_EQ(led2.reserved(a, QUOTE), 0);
  }
  s2->stop();
  cleanFiles(base);
}

// Perp: positions (qty, entry, posted margin) restore, margin is re-reserved,
// and closing the position post-restart releases it -- conservation holds.
TEST(VenueCheckpoint, PerpPositionAndMarginRestored)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_perp.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  c.baseAsset = 0;
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.maintenanceMarginBps = 500;

  Ledger led1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->start();
  s1->submit(InboundCommand{Deposit{1, QUOTE, quoteRaw(100000), SYM}});
  s1->submit(InboundCommand{Deposit{2, QUOTE, quoteRaw(100000), SYM}});
  s1->submit(InboundCommand{limit(1, Side::SELL, 100.00, 5.0, 2)});
  s1->submit(InboundCommand{limit(2, Side::BUY, 100.00, 5.0, 1)});  // +5 / -5 @ 100
  s1->submit(InboundCommand{SetMark{SYM, px(100.0)}});
  ASSERT_TRUE(s1->checkpointNow());
  s1->flush();
  const uint64_t liveHash = s1->engine().stateHash();
  const Amount margin1 = led1.reserved(1, QUOTE);
  const Amount margin2 = led1.reserved(2, QUOTE);
  ASSERT_GT(margin1, 0);
  ASSERT_GT(margin2, 0);
  s1->stop();
  s1.reset();

  Ledger led2;
  auto t2 = clockState(2'000'000);
  auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->engine().setLedger(&led2, VENUE_ACCT);
  s2->start();
  EXPECT_GT(s2->recoveredFromSnapshotRecords(), 0u);
  EXPECT_EQ(s2->engine().stateHash(), liveHash);
  EXPECT_EQ(s2->engine().positionQty(1), qty(5.0).raw());
  EXPECT_EQ(s2->engine().positionQty(2), -qty(5.0).raw());
  EXPECT_EQ(s2->engine().positionEntry(1), px(100.0));
  EXPECT_EQ(led2.reserved(1, QUOTE), margin1);
  EXPECT_EQ(led2.reserved(2, QUOTE), margin2);
  EXPECT_EQ(s2->engine().totalPositionMargin(), margin1 + margin2);

  // Close both positions at entry: margins release, totals conserve exactly.
  s2->submit(InboundCommand{limit(11, Side::BUY, 100.00, 5.0, 2)});
  s2->submit(InboundCommand{limit(12, Side::SELL, 100.00, 5.0, 1)});
  s2->flush();
  EXPECT_EQ(s2->engine().positionQty(1), 0);
  EXPECT_EQ(s2->engine().positionQty(2), 0);
  EXPECT_EQ(led2.reserved(1, QUOTE), 0);
  EXPECT_EQ(led2.reserved(2, QUOTE), 0);
  EXPECT_EQ(led2.total(1, QUOTE) + led2.total(2, QUOTE) + led2.total(VENUE_ACCT, QUOTE),
            static_cast<Amount>(quoteRaw(100000)) * 2);
  s2->stop();
  cleanFiles(base);
}

// Torn snapshot: truncating the newest snapshot at EVERY byte offset makes
// recovery fall back to the previous generation, whose snapshot + both tail
// segments reproduce the exact final state.
TEST(VenueCheckpoint, TornSnapshotFallsBackToPreviousGeneration)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_torn.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  Ledger led1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->start();
  s1->submit(InboundCommand{Deposit{1, BASE, baseRaw(100), SYM}});
  s1->submit(InboundCommand{Deposit{2, QUOTE, quoteRaw(10000), SYM}});
  s1->submit(InboundCommand{limit(1, Side::SELL, 100.00, 2.0, 1)});
  s1->submit(InboundCommand{limit(2, Side::BUY, 99.00, 1.0, 2)});
  ASSERT_TRUE(s1->checkpointNow());                                 // generation A
  s1->submit(InboundCommand{limit(3, Side::BUY, 100.00, 1.0, 2)});  // trades vs 1
  s1->submit(InboundCommand{limit(4, Side::SELL, 101.00, 1.0, 1)});
  ASSERT_TRUE(s1->checkpointNow());  // generation B
  s1->submit(InboundCommand{CancelOrder{2, SYM, 2}});
  s1->flush();
  const uint64_t liveHash = s1->engine().stateHash();
  s1->stop();
  s1.reset();

  const auto gens = SequencedShard<>::scanGenerations(base);
  ASSERT_EQ(gens.snapshots.size(), 2u);
  const std::string snapB = SequencedShard<>::snapshotPath(base, gens.snapshots[1]);

  // Pristine copy of snapshot B.
  std::vector<char> pristine;
  {
    std::ifstream in(snapB, std::ios::binary);
    pristine.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  ASSERT_GT(pristine.size(), 64u);

  for (size_t len = 0; len < pristine.size(); ++len)
  {
    {
      std::ofstream out(snapB, std::ios::binary | std::ios::trunc);
      out.write(pristine.data(), static_cast<std::streamsize>(len));
    }
    Ledger led2;
    auto t2 = clockState(9'000'000 + static_cast<int64_t>(len) * 10'000);
    // Heap-allocated: a shard embeds its 64k EventBus rings (~27 MB).
    auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                                 clockOf(t2));
    s2->engine().setLedger(&led2, VENUE_ACCT);
    s2->start();
    // Fallback still recovers THROUGH a snapshot (generation A), not a bare
    // journal replay -- and reproduces the exact final state.
    ASSERT_GT(s2->recoveredFromSnapshotRecords(), 0u) << "truncation at " << len;
    ASSERT_EQ(s2->engine().stateHash(), liveHash) << "truncation at " << len;
    ASSERT_TRUE(ledgersEqual(led2, led1, 2)) << "truncation at " << len;
    s2->stop();
  }

  // Restore pristine B: recovery prefers it again.
  {
    std::ofstream out(snapB, std::ios::binary | std::ios::trunc);
    out.write(pristine.data(), static_cast<std::streamsize>(pristine.size()));
  }
  {
    Ledger led2;
    auto t2 = clockState(50'000'000);
    auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                                 clockOf(t2));
    s2->engine().setLedger(&led2, VENUE_ACCT);
    s2->start();
    EXPECT_EQ(s2->engine().stateHash(), liveHash);
    s2->stop();
  }
  cleanFiles(base);
}

// Retention: with the default 2 generations, a third checkpoint deletes the
// first snapshot+segment pair and the legacy file; recovery from the retained
// window still reproduces the live state exactly.
TEST(VenueCheckpoint, RotationRetainsConfiguredGenerations)
{
  namespace fs = std::filesystem;
  const std::string base = "/tmp/flox_test_venue_checkpoint_rotate.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  Ledger led1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->start();
  s1->submit(InboundCommand{Deposit{1, BASE, baseRaw(100), SYM}});
  s1->submit(InboundCommand{Deposit{2, QUOTE, quoteRaw(10000), SYM}});

  std::vector<int64_t> cpTs;
  for (int gen = 0; gen < 3; ++gen)
  {
    s1->submit(InboundCommand{limit(10 + gen * 2, Side::SELL,
                                    100.00 + gen * 0.01, 1.0, 1)});
    s1->submit(InboundCommand{limit(11 + gen * 2, Side::BUY, 99.00 - gen * 0.01, 1.0, 2)});
    ASSERT_TRUE(s1->checkpointNow());
    const auto g = SequencedShard<>::scanGenerations(base);
    cpTs.push_back(g.snapshots.back());
  }
  s1->flush();
  const uint64_t liveHash = s1->engine().stateHash();
  s1->stop();
  s1.reset();

  const auto g = SequencedShard<>::scanGenerations(base);
  EXPECT_EQ(g.snapshots.size(), 2u);  // retainGenerations default = 2
  EXPECT_EQ(g.segments.size(), 2u);
  EXPECT_FALSE(fs::exists(base));  // legacy pre-checkpoint file pruned
  EXPECT_FALSE(fs::exists(SequencedShard<>::snapshotPath(base, cpTs[0])));
  EXPECT_FALSE(fs::exists(SequencedShard<>::segmentPath(base, cpTs[0])));
  EXPECT_TRUE(fs::exists(SequencedShard<>::snapshotPath(base, cpTs[1])));
  EXPECT_TRUE(fs::exists(SequencedShard<>::snapshotPath(base, cpTs[2])));

  // The pre-checkpoint history is GONE (legacy pruned): state equality below
  // is provable ONLY if recovery consumed a snapshot -- deposits exist
  // nowhere else. The counter makes that explicit.
  Ledger led2;
  auto t2 = clockState(9'000'000);
  auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->engine().setLedger(&led2, VENUE_ACCT);
  s2->start();
  EXPECT_GT(s2->recoveredFromSnapshotRecords(), 0u);
  EXPECT_EQ(s2->engine().stateHash(), liveHash);
  EXPECT_TRUE(ledgersEqual(led2, led1, 2));
  s2->stop();
  cleanFiles(base);
}

// Snapshot-only records must never apply from live traffic: a client that
// could submit RestoreOrder would mint itself a resting order.
TEST(VenueCheckpoint, LiveSubmitOfSnapshotRecordIsDropped)
{
  // Engine level.
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  RestoreOrder forged{};
  forged.id = 42;
  forged.accountId = 7;
  forged.price = px(100.0);
  forged.leaves = qty(5.0);
  forged.side = Side::BUY;
  eng.submit(InboundCommand{forged}, 1000);
  EXPECT_EQ(eng.droppedSnapshotRecords(), 1u);
  EXPECT_FALSE(eng.book().contains(42));
  eng.submit(InboundCommand{SnapshotEnd{}}, 2000);
  EXPECT_EQ(eng.droppedSnapshotRecords(), 2u);
  EXPECT_EQ(eng.tradesGenerated(), 0u);

  // Shard level: journaled or not, the record never materializes -- including
  // across a restart replay.
  const std::string base = "/tmp/flox_test_venue_checkpoint_forged.bin";
  cleanFiles(base);
  {
    auto t = clockState(1'000'000);
    auto s = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off,
                                                clockOf(t));
    s->start();
    s->submit(InboundCommand{forged});
    s->flush();
    EXPECT_EQ(s->engine().droppedSnapshotRecords(), 1u);
    EXPECT_FALSE(s->engine().book().contains(42));
    s->stop();
  }
  {
    auto t = clockState(2'000'000);
    auto s = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off,
                                                clockOf(t));
    s->start();  // replay drops it again, deterministically
    EXPECT_FALSE(s->engine().book().contains(42));
    s->stop();
  }
  cleanFiles(base);
}

// Pause measurement (no threshold assert -- just the numbers) on a ~100k-order
// book. With the asynchronous checkpoint the consumer pause is the CLONE time
// (cloneForSnapshot); serialization+fsync runs in the background against the
// clone. Both are measured: the clone must be a small fraction of the old
// synchronous writeSnapshot pause, and the clone must be state-identical.
TEST(VenueCheckpoint, SnapshotPauseMeasuredOn100kOrders)
{
  const std::string path = "/tmp/flox_test_venue_checkpoint_pause.snap";
  std::remove(path.c_str());

  venue::SymbolConfig c = cfg();
  Ledger led;
  MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  for (int a = 1; a <= NACCT; ++a)
  {
    eng.submit(InboundCommand{Deposit{static_cast<uint64_t>(a), BASE, baseRaw(10'000'000), SYM}}, a);
    eng.submit(InboundCommand{Deposit{static_cast<uint64_t>(a), QUOTE, quoteRaw(1'000'000'000), SYM}},
               NACCT + a);
  }
  Rng rng{0xBEEFCAFEULL};
  const int64_t tickRaw = px(0.01).raw();
  const int kOrders = 100'000;
  for (int i = 0; i < kOrders; ++i)
  {
    const uint64_t r = rng.next();
    NewOrder o;
    o.id = static_cast<OrderId>(i + 1);
    o.symbol = SYM;
    o.side = (r & 1) ? Side::BUY : Side::SELL;
    o.accountId = 1 + (r >> 8) % NACCT;
    // Non-crossing: bids in [60, 90), asks in [110, 140).
    const int64_t off = static_cast<int64_t>((r >> 16) % 3000);
    o.price = (r & 1) ? Price::fromRaw(px(60.0).raw() + off * tickRaw)
                      : Price::fromRaw(px(110.0).raw() + off * tickRaw);
    o.quantity = qty(1.0 + static_cast<double>((r >> 40) % 3));
    o.type = OrderType::LIMIT;
    eng.submit(InboundCommand{o}, 100 + i);
  }
  ASSERT_GT(eng.restingOrderCount(), 90'000u);

  // Old synchronous pause (reference): serialize the live engine directly.
  uint64_t bytes = 0;
  const auto t0 = std::chrono::steady_clock::now();
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    eng.writeSnapshot(out);
    bytes = out.bytes();
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double serializeMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();

  // New pause: the clone taken under the consumer pause of the async path.
  const auto c0 = std::chrono::steady_clock::now();
  auto clone = eng.cloneForSnapshot();
  const auto c1 = std::chrono::steady_clock::now();
  const double cloneMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(c1 - c0).count();

  // The clone is state-identical: the background writer serializes exactly
  // what the live engine held at the boundary.
  EXPECT_EQ(clone.engine->stateHash(), eng.stateHash());
  EXPECT_EQ(clone.engine->restingOrderCount(), eng.restingOrderCount());
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    clone.engine->writeSnapshot(out);
    EXPECT_EQ(out.bytes(), bytes);
  }

  std::printf("[ PAUSE    ] writeSnapshot (old sync pause): %zu resting orders, %.1f MiB, %.2f ms\n",
              static_cast<size_t>(eng.restingOrderCount()),
              static_cast<double>(bytes) / (1024.0 * 1024.0), serializeMs);
  std::printf("[ PAUSE    ] cloneForSnapshot (new async pause): %.2f ms (%.1fx shorter)\n",
              cloneMs, serializeMs / (cloneMs > 0.0 ? cloneMs : 1e-9));
  EXPECT_LT(cloneMs, serializeMs);  // the async pause must beat the serialize pause
  std::remove(path.c_str());
}

// Conservation fuzz with periodic checkpoints: checkpointing at arbitrary
// command boundaries must be invisible to the money-conservation invariant,
// and a final recovery must reproduce the ledger split exactly, draining to
// zero reserved.
TEST(VenueCheckpoint, ConservationHoldsAcrossPeriodicCheckpoints)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_fuzz.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  c.lastLookWindowNs = 40'000;
  c.lastLookAcceptOnTimeout = true;  // timeout-accepts settle like real fills

  Ledger led1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->start();
  for (int a = 1; a <= NACCT; ++a)
  {
    s1->submit(InboundCommand{Deposit{static_cast<uint64_t>(a), BASE, baseRaw(1000), SYM}});
    s1->submit(InboundCommand{Deposit{static_cast<uint64_t>(a), QUOTE, quoteRaw(100000), SYM}});
  }
  const Amount initBase = static_cast<Amount>(baseRaw(1000)) * NACCT;
  const Amount initQuote = static_cast<Amount>(quoteRaw(100000)) * NACCT;
  auto sumAsset = [&](const Ledger& led, AssetId asset)
  {
    Amount t = led.total(VENUE_ACCT, asset);
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, asset);
    }
    return t;
  };

  Rng rng{0xA5A5A5A51234ULL};
  OrderId nextId = 1;
  const int N = 20'000;
  int checkpoints = 0;
  for (int i = 0; i < N; ++i)
  {
    if (i > 0 && i % 4000 == 0)
    {
      ASSERT_TRUE(s1->checkpointNow());
      ++checkpoints;
      EXPECT_EQ(sumAsset(led1, BASE), initBase) << "at checkpoint after op " << i;
      EXPECT_EQ(sumAsset(led1, QUOTE), initQuote) << "at checkpoint after op " << i;
    }
    s1->submit(randomCmd(rng, nextId, i, /*moneyFlow*/ false));
  }
  s1->flush();
  EXPECT_EQ(checkpoints, 4);
  EXPECT_EQ(sumAsset(led1, BASE), initBase);
  EXPECT_EQ(sumAsset(led1, QUOTE), initQuote);
  s1->stop();
  s1.reset();

  // Recovery reproduces the split; conservation holds; draining releases all.
  Ledger led2;
  auto t2 = clockState(t1->load() + 1'000'000);
  auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->engine().setLedger(&led2, VENUE_ACCT);
  s2->start();
  EXPECT_TRUE(ledgersEqual(led2, led1, NACCT));
  EXPECT_EQ(sumAsset(led2, BASE), initBase);
  EXPECT_EQ(sumAsset(led2, QUOTE), initQuote);

  for (OrderId id = 1; id < nextId; ++id)
  {
    s2->submit(InboundCommand{CancelOrder{id, SYM, 0}});
  }
  // Cancel-while-held resolves holds; sweep any residual timeout holds too.
  t2->fetch_add(100'000'000);
  s2->submit(InboundCommand{TimeTick{SYM}});
  s2->flush();
  EXPECT_EQ(sumAsset(led2, BASE), initBase);
  EXPECT_EQ(sumAsset(led2, QUOTE), initQuote);
  for (int a = 1; a <= NACCT; ++a)
  {
    EXPECT_EQ(led2.reserved(a, BASE), 0);
    EXPECT_EQ(led2.reserved(a, QUOTE), 0);
  }
  s2->stop();
  cleanFiles(base);
}

// BalanceUpdate across checkpoint/recovery: a live deposit emits exactly one
// BalanceUpdate; recovery (snapshot Deposit records + journal tail) publishes
// NOTHING outbound -- reconnecting clients reconcile via snapshots, not a
// re-broadcast; a fresh post-recovery deposit emits exactly one again. The
// onCheckpoint hook fires at the checkpoint boundary and the FIX session
// sidecar written there restores into a fresh host + registry.
TEST(VenueCheckpoint, BalanceUpdateRecoverySuppressionAndSidecarHook)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_balance.bin";
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
TEST(VenueCheckpoint, NegativeAvailableBalanceRestoredExactly)
{
  const std::string path = "/tmp/flox_test_venue_checkpoint_negbal.snap";
  std::remove(path.c_str());

  venue::SymbolConfig c = cfg();
  Ledger led;
  MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  eng.submit(InboundCommand{Deposit{1, QUOTE, quoteRaw(100.0), SYM}}, 1000);
  eng.submit(InboundCommand{Deposit{2, BASE, baseRaw(10.0), SYM}}, 2000);
  // Account 2 keeps a live reservation (resting ask) on top of the distortion.
  eng.submit(InboundCommand{limit(1, Side::SELL, 100.00, 3.0, 2)}, 3000);
  // Force the "impossible" moments directly (the liquidation path produces the
  // same shape through funding/fees; the ledger is the state being tested):
  // acct 1 wallet negative, acct 3 negative available with zero total.
  led.credit(1, QUOTE, -static_cast<Amount>(quoteRaw(150.0)));  // avail = -50
  led.credit(3, QUOTE, -static_cast<Amount>(quoteRaw(7.0)));    // avail = -7, no deposit at all
  ASSERT_LT(led.available(1, QUOTE), 0);
  ASSERT_GT(led.reserved(2, BASE), 0);

  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    eng.writeSnapshot(out);
    out.flush();
  }

  Ledger led2;
  MatchingEngine<MatchingBook> rec(c, [](const OutboundEvent&) {});
  rec.setLedger(&led2, VENUE_ACCT);
  for (const auto& [ts, cmd] : Journal::loadTimed(path))
  {
    ASSERT_TRUE(rec.applySnapshotRecord(cmd, ts));  // incl. the SnapshotEnd hash check
  }
  EXPECT_EQ(rec.stateHash(), eng.stateHash());
  EXPECT_EQ(led2.available(1, QUOTE), led.available(1, QUOTE));  // exact, negative
  EXPECT_EQ(led2.available(3, QUOTE), led.available(3, QUOTE));
  EXPECT_EQ(led2.available(2, BASE), led.available(2, BASE));
  EXPECT_EQ(led2.reserved(2, BASE), led.reserved(2, BASE));  // reservation split intact
  // The restored reservation is live: cancel releases it back to available.
  rec.submit(InboundCommand{CancelOrder{1, SYM, 2}}, 9000);
  EXPECT_EQ(led2.reserved(2, BASE), 0);
  EXPECT_EQ(led2.available(2, BASE), static_cast<Amount>(baseRaw(10.0)));
  std::remove(path.c_str());
}

// Backward read compatibility: Deposit records inside a snapshot (the v1
// balance encoding) still apply through the journaled-deposit path, and the
// reserved side reconstitutes by re-reservation out of the deposited total.
TEST(VenueCheckpoint, LegacyDepositSnapshotRecordsStillApply)
{
  const std::string path = "/tmp/flox_test_venue_checkpoint_legacy.snap";
  std::remove(path.c_str());
  const int64_t ts = 5000;

  venue::SymbolConfig c = cfg();

  // Reference pass: apply the legacy-style body to one engine to learn the
  // state hash the crafted SnapshotEnd must carry.
  RestoreReservation rr{};
  rr.id = 5;
  rr.account = 1;
  rr.asset = QUOTE;
  rr.side = Side::BUY;
  rr.limitPriceRaw = px(100.0).raw();
  rr.reservedRaw = static_cast<Amount>(quoteRaw(300.0));
  const Deposit dep{1, QUOTE, quoteRaw(1000.0), SYM};

  Ledger refLed;
  MatchingEngine<MatchingBook> ref(c, [](const OutboundEvent&) {});
  ref.setLedger(&refLed, VENUE_ACCT);
  ASSERT_TRUE(ref.applySnapshotRecord(InboundCommand{dep}, ts));
  ASSERT_TRUE(ref.applySnapshotRecord(InboundCommand{rr}, ts));
  const uint64_t h = ref.stateHash();

  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    out.append(InboundCommand{SnapshotBegin{kSnapshotFormatVersion, ts, h, 0}}, ts);
    out.append(InboundCommand{dep}, ts);
    out.append(InboundCommand{rr}, ts);
    SnapshotEnd end{};
    end.stateHash = h;
    end.nowNs = ts;
    out.append(InboundCommand{end}, ts);
    out.flush();
  }

  Ledger led2;
  MatchingEngine<MatchingBook> rec(c, [](const OutboundEvent&) {});
  rec.setLedger(&led2, VENUE_ACCT);
  for (const auto& [rts, cmd] : Journal::loadTimed(path))
  {
    ASSERT_TRUE(rec.applySnapshotRecord(cmd, rts));  // SnapshotEnd verifies the hash
  }
  // The deposited TOTAL split back into available/reserved by re-reservation.
  EXPECT_EQ(led2.available(1, QUOTE), static_cast<Amount>(quoteRaw(700.0)));
  EXPECT_EQ(led2.reserved(1, QUOTE), static_cast<Amount>(quoteRaw(300.0)));
  EXPECT_EQ(rec.stateHash(), h);
  std::remove(path.c_str());
}

// MMP fill windows restore EXACTLY: a maker one fill from its qtyLimit before
// the checkpoint is still one fill from it after recovery -- the next fill
// trips the breach precisely as it would have without the restart.
TEST(VenueCheckpoint, MmpWindowRestoredExactly)
{
  const std::string path = "/tmp/flox_test_venue_checkpoint_mmpwin.snap";
  std::remove(path.c_str());

  venue::SymbolConfig c = cfg();
  std::vector<OutboundEvent> evA;
  MatchingEngine<MatchingBook> a(c, [&](const OutboundEvent& e)
                                 { evA.push_back(e); });
  a.setMmp(1, qty(10.0), 1'000'000'000);

  // Two fills, 9.0 total inside the window: one unit below the limit.
  a.submit(InboundCommand{limit(1, Side::SELL, 100.00, 4.0, 1)}, 1000);
  a.submit(InboundCommand{limit(2, Side::BUY, 100.00, 4.0, 2)}, 2000);
  a.submit(InboundCommand{limit(3, Side::SELL, 100.00, 5.0, 1)}, 3000);
  a.submit(InboundCommand{limit(4, Side::BUY, 100.00, 5.0, 2)}, 4000);
  // A resting quote that the breach must pull after recovery.
  a.submit(InboundCommand{limit(5, Side::SELL, 101.00, 2.0, 1)}, 5000);
  for (const auto& e : evA)
  {
    ASSERT_EQ(std::get_if<MmpTriggered>(&e), nullptr);  // not breached yet
  }

  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    a.writeSnapshot(out);
    out.flush();
  }
  std::vector<OutboundEvent> evB;
  MatchingEngine<MatchingBook> b(c, [&](const OutboundEvent& e)
                                 { evB.push_back(e); });
  for (const auto& [ts, cmd] : Journal::loadTimed(path))
  {
    ASSERT_TRUE(b.applySnapshotRecord(cmd, ts));
  }
  ASSERT_EQ(b.stateHash(), a.stateHash());

  // Identical next fill into both: 1.0 more -> sum 10 >= limit -> breach, on
  // the live engine AND the recovered one, at the same point.
  const auto trigger = [](MatchingEngine<MatchingBook>& e, std::vector<OutboundEvent>& ev)
  {
    e.submit(InboundCommand{limit(6, Side::SELL, 100.00, 1.0, 1)}, 6000);
    e.submit(InboundCommand{limit(7, Side::BUY, 100.00, 1.0, 2)}, 7000);
    bool mmp = false, pulled = false;
    for (const auto& x : ev)
    {
      if (const auto* m = std::get_if<MmpTriggered>(&x); m && m->accountId == 1)
      {
        mmp = true;
      }
      if (const auto* cxl = std::get_if<OrderCanceled>(&x); cxl && cxl->id == 5)
      {
        pulled = true;
      }
    }
    return mmp && pulled;
  };
  EXPECT_TRUE(trigger(a, evA));
  EXPECT_TRUE(trigger(b, evB));
  EXPECT_EQ(b.stateHash(), a.stateHash());
  std::remove(path.c_str());
}

// STP groups: a runtime SetStpGroup is a sequenced, journaled command; the
// table survives a snapshot restore AND a journal-tail replay, and firm-group
// STP keeps firing after recovery.
TEST(VenueCheckpoint, StpGroupsJournaledSnapshottedAndRestored)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_stp.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  const auto stpCanceled = [](const std::vector<OutboundEvent>& events, OrderId id)
  {
    for (const auto& e : events)
    {
      if (const auto* x = std::get_if<OrderCanceled>(&e);
          x && x->id == id && x->reason == CancelReason::SelfTradePrevention)
      {
        return true;
      }
    }
    return false;
  };

  HashSink sink1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->subscribeOutbound(&sink1);
  s1->start();
  s1->submit(InboundCommand{SetStpGroup{SYM, 1, 77}});
  s1->submit(InboundCommand{SetStpGroup{SYM, 2, 77}});
  s1->submit(InboundCommand{limit(1, Side::SELL, 100.00, 1.0, 1)});
  NewOrder tk = limit(2, Side::BUY, 100.00, 1.0, 2);
  tk.stp = STPMode::CancelNewest;
  s1->submit(InboundCommand{tk});  // same firm group: canceled, never trades
  s1->flush();
  EXPECT_TRUE(stpCanceled(sink1.events, 2));
  ASSERT_TRUE(s1->checkpointNow());
  s1->submit(InboundCommand{SetStpGroup{SYM, 3, 77}});  // journal-tail mutation
  s1->flush();
  const uint64_t liveHash = s1->engine().stateHash();
  s1->stop();
  s1.reset();

  HashSink sink2;
  auto t2 = clockState(2'000'000);
  auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->subscribeOutbound(&sink2);
  s2->start();
  EXPECT_GT(s2->recoveredFromSnapshotRecords(), 0u);
  EXPECT_EQ(s2->engine().stateHash(), liveHash);  // incl. the STP-group fold

  // Snapshot-carried membership (acct 2) and tail-replayed membership (acct 3)
  // both still fire; an ungrouped account trades normally.
  NewOrder tk2 = limit(3, Side::BUY, 100.00, 1.0, 2);
  tk2.stp = STPMode::CancelNewest;
  s2->submit(InboundCommand{tk2});
  NewOrder tk3 = limit(4, Side::BUY, 100.00, 1.0, 3);
  tk3.stp = STPMode::CancelNewest;
  s2->submit(InboundCommand{tk3});
  s2->flush();
  EXPECT_TRUE(stpCanceled(sink2.events, 3));
  EXPECT_TRUE(stpCanceled(sink2.events, 4));
  bool traded = false;
  NewOrder tk4 = limit(5, Side::BUY, 100.00, 1.0, 9);  // no group: crosses maker 1
  tk4.stp = STPMode::CancelNewest;
  s2->submit(InboundCommand{tk4});
  s2->flush();
  for (const auto& e : sink2.events)
  {
    if (const auto* t = std::get_if<venue::Trade>(&e); t && t->takerId == 5 && t->makerId == 1)
    {
      traded = true;
    }
  }
  EXPECT_TRUE(traded);
  s2->stop();
  cleanFiles(base);
}

// Constructor-config guard: a snapshot restored into an engine built with
// different structural parameters (here: another qtyScale) is rejected at
// SnapshotBegin, and shard recovery falls back to full-history replay instead
// of silently reinterpreting fixed-point state.
TEST(VenueCheckpoint, ConfigHashMismatchRejectsSnapshot)
{
  const std::string base = "/tmp/flox_test_venue_checkpoint_cfghash.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  {
    auto t1 = clockState(1'000'000);
    auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                                 clockOf(t1));
    s1->start();
    s1->submit(InboundCommand{limit(1, Side::SELL, 100.00, 2.0, 1)});
    ASSERT_TRUE(s1->checkpointNow());
    s1->flush();
    s1->stop();
  }

  // Engine level: SnapshotBegin refuses the foreign config outright.
  venue::SymbolConfig other = cfg();
  other.qtyScale = 1'000'000;  // valid scale, different from the writer's 1e8
  {
    const auto gens = SequencedShard<>::scanGenerations(base);
    ASSERT_EQ(gens.snapshots.size(), 1u);
    const auto records =
        Journal::loadTimed(SequencedShard<>::snapshotPath(base, gens.snapshots[0]));
    ASSERT_FALSE(records.empty());
    MatchingEngine<MatchingBook> alien(other, [](const OutboundEvent&) {});
    EXPECT_FALSE(alien.applySnapshotRecord(records.front().second, records.front().first));
    MatchingEngine<MatchingBook> same(c, [](const OutboundEvent&) {});
    EXPECT_TRUE(same.applySnapshotRecord(records.front().second, records.front().first));
  }

  // Shard level: recovery rejects the snapshot (config mismatch) and falls
  // back to replaying the full retained history.
  {
    auto t2 = clockState(9'000'000);
    auto s2 = std::make_unique<SequencedShard<>>(other, base, MatchingBook{}, Journal::Sync::Off,
                                                 clockOf(t2));
    s2->start();
    EXPECT_EQ(s2->recoveredFromSnapshotRecords(), 0u);  // snapshot refused
    EXPECT_GT(s2->recoveredCommands(), 0u);             // full-history replay ran
    s2->stop();
  }
  cleanFiles(base);
}

// THE async-checkpoint crash window: the journal rotates at the clone boundary
// while the snapshot publishes in the background, so a crash mid-serialization
// leaves "segment ts on disk, snapshot ts absent (a torn .tmp at most)".
// Recovery must ignore the .tmp, fall back to the previous snapshot and replay
// BOTH tail segments after it -- reproducing the live state exactly.
TEST(VenueCheckpoint, CrashBeforeSnapshotPublishRecoversViaPreviousGeneration)
{
  namespace fs = std::filesystem;
  const std::string base = "/tmp/flox_test_venue_checkpoint_crashpub.bin";
  cleanFiles(base);

  venue::SymbolConfig c = cfg();
  Ledger led1;
  auto t1 = clockState(1'000'000);
  auto s1 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t1));
  s1->engine().setLedger(&led1, VENUE_ACCT);
  s1->start();
  s1->submit(InboundCommand{Deposit{1, BASE, baseRaw(100), SYM}});
  s1->submit(InboundCommand{Deposit{2, QUOTE, quoteRaw(10000), SYM}});
  s1->submit(InboundCommand{limit(1, Side::SELL, 100.00, 2.0, 1)});
  ASSERT_TRUE(s1->checkpointNow());                                 // generation A
  s1->submit(InboundCommand{limit(2, Side::BUY, 100.00, 1.0, 2)});  // trades vs 1
  ASSERT_TRUE(s1->checkpointNow());                                 // generation B
  s1->submit(InboundCommand{limit(3, Side::BUY, 99.00, 1.0, 2)});   // tail after B
  s1->flush();
  EXPECT_GT(s1->lastCheckpointPauseNs(), 0);  // the async pause gauge moved
  const uint64_t liveHash = s1->engine().stateHash();
  s1->stop();
  s1.reset();

  const auto gens = SequencedShard<>::scanGenerations(base);
  ASSERT_EQ(gens.snapshots.size(), 2u);
  ASSERT_EQ(gens.segments.size(), 2u);
  const std::string snapA = SequencedShard<>::snapshotPath(base, gens.snapshots[0]);
  const std::string snapB = SequencedShard<>::snapshotPath(base, gens.snapshots[1]);

  // Simulate the crash: snapshot B never renamed in; only a torn .tmp exists.
  {
    std::vector<char> bytes;
    {
      std::ifstream in(snapB, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    fs::remove(snapB);
    std::ofstream tmp(snapB + ".tmp", std::ios::binary | std::ios::trunc);
    tmp.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
  }

  Ledger led2;
  auto t2 = clockState(9'000'000);
  auto s2 = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off,
                                               clockOf(t2));
  s2->engine().setLedger(&led2, VENUE_ACCT);
  s2->start();
  // Recovery went through snapshot A (the .tmp is not a generation), then
  // replayed segment A AND segment B as the tail.
  const size_t snapARecords = Journal::loadTimed(snapA).size();
  const size_t segARecords =
      Journal::loadTimed(SequencedShard<>::segmentPath(base, gens.segments[0])).size();
  const size_t segBRecords =
      Journal::loadTimed(SequencedShard<>::segmentPath(base, gens.segments[1])).size();
  EXPECT_EQ(s2->recoveredFromSnapshotRecords(), snapARecords);
  EXPECT_EQ(s2->recoveredCommands(), snapARecords + segARecords + segBRecords);
  EXPECT_EQ(s2->engine().stateHash(), liveHash);
  EXPECT_TRUE(ledgersEqual(led2, led1, 2));

  // The revived shard checkpoints normally on top of the fallback.
  ASSERT_TRUE(s2->checkpointNow());
  s2->stop();
  cleanFiles(base);
  std::error_code ec;
  fs::remove(snapB + ".tmp", ec);
}
