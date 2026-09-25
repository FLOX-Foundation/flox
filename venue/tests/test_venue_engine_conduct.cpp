/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The conduct components, each on its own: the clientOrderId dedup window,
// self-trade prevention, GTD deadlines, pegs and market-maker protection.
//
// These carry the state and the decisions that used to sit inside
// MatchingEngine<Book> and could only be reached through a whole engine, a
// book and a sink. Reached directly they can be asked the questions an
// end-to-end test cannot put cheaply: what the peg target is when one side of
// the book is missing, whether an expiry is due exactly ON its deadline,
// which leg a self-matching auction pair loses. The engine-level behaviour
// these feed is covered where it always was (the golden replay, the auction,
// checkpoint and clOrdId-window tests) -- this file is about the decisions,
// not about the plumbing.

#include "flox-venue/engine/clordid_window.h"
#include "flox-venue/engine/expiry.h"
#include "flox-venue/engine/mmp.h"
#include "flox-venue/engine/pegs.h"
#include "flox-venue/engine/stp.h"
#include "flox-venue/journal.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SeqNanos at(int64_t ns) { return SeqNanos::fromRaw(ns); }

// A journal on a throwaway path, read back as records. The components write
// snapshot sections; what the tests check is WHICH records come out and in
// what order, which is the part that has to stay byte-for-byte stable.
class TempJournal
{
 public:
  explicit TempJournal(const std::string& stem) : path_(flox::venue::test::tmpPath(stem, ".bin")) {}

  ~TempJournal() { std::remove(path_.c_str()); }

  TempJournal(const TempJournal&) = delete;
  TempJournal& operator=(const TempJournal&) = delete;

  template <class Fn>
  std::vector<InboundCommand> write(Fn&& fn)
  {
    {
      Journal j(path_);
      fn(j);
    }
    std::vector<InboundCommand> out;
    for (const auto& [ts, cmd] : Journal::loadTimed(path_))
    {
      (void)ts;
      out.push_back(cmd);
    }
    return out;
  }

 private:
  std::string path_;
};

// ---- clientOrderId dedup window ------------------------------------------

TEST(ConductClOrdIdWindow, ARepeatedIdIsRefused)
{
  ClOrdIdWindow w;
  EXPECT_FALSE(w.duplicate(1, 77, 1000, 0));
  EXPECT_TRUE(w.duplicate(1, 77, 1001, 0));
  EXPECT_TRUE(w.duplicate(1, 77, 2000, 0));
}

TEST(ConductClOrdIdWindow, IdZeroMeansUnsetAndIsExempt)
{
  ClOrdIdWindow w;
  EXPECT_FALSE(w.duplicate(1, 0, 1000, 0));
  EXPECT_FALSE(w.duplicate(1, 0, 1001, 0));
}

TEST(ConductClOrdIdWindow, TheIndexIsPerAccount)
{
  ClOrdIdWindow w;
  EXPECT_FALSE(w.duplicate(1, 77, 1000, 0));
  EXPECT_FALSE(w.duplicate(2, 77, 1000, 0));
  EXPECT_TRUE(w.duplicate(1, 77, 1000, 0));
}

TEST(ConductClOrdIdWindow, PastTwoWindowsAnOldIdIsAcceptedAgain)
{
  ClOrdIdWindow w;
  const int64_t window = 1000;
  EXPECT_FALSE(w.duplicate(1, 77, 0, window));  // first touch arms the rotation clock
  EXPECT_TRUE(w.duplicate(1, 77, 500, window));
  // One rotation: the id moves to the previous generation and still blocks.
  EXPECT_TRUE(w.duplicate(1, 77, 1500, window));
  // A second rotation drops the generation that held it.
  EXPECT_FALSE(w.duplicate(1, 99, 3000, window));
  EXPECT_FALSE(w.duplicate(1, 77, 3000, window));
}

TEST(ConductClOrdIdWindow, AnUnboundedWindowNeverForgets)
{
  ClOrdIdWindow w;
  EXPECT_FALSE(w.duplicate(1, 77, 0, 0));
  EXPECT_TRUE(w.duplicate(1, 77, 86'400'000'000'000LL * 365, 0));
}

TEST(ConductClOrdIdWindow, TheHashIgnoresTheInsertionOrder)
{
  ClOrdIdWindow a;
  ClOrdIdWindow b;
  for (uint64_t id : {5U, 1U, 9U, 3U})
  {
    a.duplicate(7, id, 10, 0);
  }
  for (uint64_t id : {9U, 3U, 5U, 1U})
  {
    b.duplicate(7, id, 10, 0);
  }
  EXPECT_EQ(a.hashInto(0), b.hashInto(0));
}

TEST(ConductClOrdIdWindow, TheSplitBetweenGenerationsIsPartOfTheHash)
{
  ClOrdIdWindow rotated;
  rotated.duplicate(7, 1, 100, 1000);   // arms the rotation clock at 100
  rotated.duplicate(7, 2, 1500, 1000);  // a window later: 1 in prev, 2 in cur

  ClOrdIdWindow flat;
  flat.duplicate(7, 1, 0, 0);
  flat.duplicate(7, 2, 0, 0);  // both in cur

  EXPECT_NE(rotated.hashInto(0), flat.hashInto(0));
}

TEST(ConductClOrdIdWindow, TheSnapshotBatchesEachGenerationSeparately)
{
  ClOrdIdWindow w;
  w.duplicate(7, 1, 100, 1000);
  w.duplicate(7, 2, 1500, 1000);  // rotates: 1 moves to the previous half
  w.duplicate(8, 3, 1500, 1000);

  TempJournal tj("conduct-clordid");
  const auto recs = tj.write([&](Journal& j)
                             { w.writeSnapshot(j, 42); });

  ASSERT_EQ(recs.size(), 3U);
  std::vector<std::tuple<uint64_t, uint32_t, uint64_t>> got;
  for (const auto& c : recs)
  {
    const auto* r = std::get_if<RestoreClOrdIds>(&c);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(r->count, 1U);
    got.emplace_back(r->account, r->generation, r->ids[0]);
  }
  EXPECT_EQ(got[0], std::make_tuple(7ULL, 0U, 2ULL));  // account 7, current half
  EXPECT_EQ(got[1], std::make_tuple(7ULL, 1U, 1ULL));  // account 7, previous half
  EXPECT_EQ(got[2], std::make_tuple(8ULL, 0U, 3ULL));  // account 8 follows
}

TEST(ConductClOrdIdWindow, RestoreReproducesTheWindowTheSnapshotDescribed)
{
  ClOrdIdWindow w;
  w.duplicate(7, 1, 100, 1000);
  w.duplicate(7, 2, 1500, 1000);

  TempJournal tj("conduct-clordid-restore");
  const auto recs = tj.write([&](Journal& j)
                             { w.writeSnapshot(j, 42); });

  ClOrdIdWindow back;
  for (const auto& c : recs)
  {
    const auto* r = std::get_if<RestoreClOrdIds>(&c);
    ASSERT_NE(r, nullptr);
    back.restore(r->account, r->generation, r->ids, r->count, r->rotatedAtNs);
  }
  // The rotation moment rides the record too (the snapshot round-trip fix), so the restored
  // window is the SAME state and not merely one that blocks the same ids --
  // which is what the engine's own SnapshotEnd hash check demands.
  EXPECT_EQ(back.hashInto(0), w.hashInto(0));
  EXPECT_TRUE(back.duplicate(7, 1, 1500, 0));
  EXPECT_TRUE(back.duplicate(7, 2, 1500, 0));
  EXPECT_FALSE(back.duplicate(7, 3, 1500, 0));
}

// ---- self-trade prevention ------------------------------------------------

TEST(ConductStp, CancelNewestPullsTheRequestersOwnLeg)
{
  const auto v = StpState::auctionVerdict(STPMode::CancelNewest, STPMode::None);
  EXPECT_TRUE(v.engaged);
  EXPECT_FALSE(v.decrement);
  EXPECT_TRUE(v.cancelBid);   // the bid asked: its own leg is the "newest"
  EXPECT_FALSE(v.cancelAsk);  // the resting counterparty survives
}

TEST(ConductStp, CancelOldestPullsTheCounterparty)
{
  const auto v = StpState::auctionVerdict(STPMode::CancelOldest, STPMode::None);
  EXPECT_TRUE(v.engaged);
  EXPECT_FALSE(v.cancelBid);
  EXPECT_TRUE(v.cancelAsk);
}

TEST(ConductStp, TheAskSideIsMirrored)
{
  const auto newest = StpState::auctionVerdict(STPMode::None, STPMode::CancelNewest);
  EXPECT_TRUE(newest.cancelAsk);
  EXPECT_FALSE(newest.cancelBid);

  const auto oldest = StpState::auctionVerdict(STPMode::None, STPMode::CancelOldest);
  EXPECT_TRUE(oldest.cancelBid);
  EXPECT_FALSE(oldest.cancelAsk);
}

TEST(ConductStp, CancelBothTakesTheWholePair)
{
  const auto v = StpState::auctionVerdict(STPMode::CancelBoth, STPMode::None);
  EXPECT_TRUE(v.cancelBid);
  EXPECT_TRUE(v.cancelAsk);
}

TEST(ConductStp, WhenBothLegsAskTheCancellationsUnion)
{
  const auto v = StpState::auctionVerdict(STPMode::CancelNewest, STPMode::CancelNewest);
  EXPECT_TRUE(v.cancelBid);
  EXPECT_TRUE(v.cancelAsk);
}

TEST(ConductStp, DecrementOutranksTheCancels)
{
  const auto v = StpState::auctionVerdict(STPMode::Decrement, STPMode::CancelBoth);
  EXPECT_TRUE(v.engaged);
  EXPECT_TRUE(v.decrement);
  EXPECT_FALSE(v.cancelBid);
  EXPECT_FALSE(v.cancelAsk);
}

TEST(ConductStp, NeitherLegAskingLeavesThePairToPrint)
{
  const auto v = StpState::auctionVerdict(STPMode::None, STPMode::None);
  EXPECT_FALSE(v.engaged);
  EXPECT_FALSE(v.decrement);
  EXPECT_FALSE(v.cancelBid);
  EXPECT_FALSE(v.cancelAsk);
}

TEST(ConductStp, ScopeIsTheFirmGroupWhenThereIsOne)
{
  std::unordered_map<uint64_t, uint64_t> groups;
  EXPECT_EQ(StpState::scopeOf(groups, 11), 11U);  // empty table: account-level
  groups[11] = 900;
  groups[12] = 900;
  EXPECT_EQ(StpState::scopeOf(groups, 11), 900U);
  EXPECT_EQ(StpState::scopeOf(groups, 12), 900U);
  EXPECT_EQ(StpState::scopeOf(groups, 13), 13U);  // absent: account-level
}

TEST(ConductStp, NoneIsNotStoredAndClearsAReusedId)
{
  StpState s;
  EXPECT_EQ(s.modeOf(5), STPMode::None);
  s.track(5, STPMode::CancelBoth);
  EXPECT_EQ(s.modeOf(5), STPMode::CancelBoth);
  s.track(5, STPMode::None);
  EXPECT_EQ(s.modeOf(5), STPMode::None);
  EXPECT_EQ(s.hashInto(0), StpState{}.hashInto(0));

  s.track(6, STPMode::Decrement);
  s.forget(6);
  EXPECT_EQ(s.hashInto(0), StpState{}.hashInto(0));
}

TEST(ConductStp, TheSnapshotIsWrittenInOrderIdOrder)
{
  StpState s;
  s.track(9, STPMode::CancelNewest);
  s.track(2, STPMode::CancelOldest);
  s.track(5, STPMode::Decrement);
  s.restore(7, STPMode::None);  // None is not state: nothing is stored

  TempJournal tj("conduct-stp");
  const auto recs = tj.write([&](Journal& j)
                             { s.writeSnapshot(j, 42); });

  ASSERT_EQ(recs.size(), 3U);
  std::vector<OrderId> ids;
  for (const auto& c : recs)
  {
    const auto* r = std::get_if<RestoreOrderStp>(&c);
    ASSERT_NE(r, nullptr);
    ids.push_back(r->id);
  }
  EXPECT_EQ(ids, (std::vector<OrderId>{2, 5, 9}));
}

// ---- GTD deadlines --------------------------------------------------------

TEST(ConductExpiry, AnOrderIsDueOnItsDeadlineNotAfterIt)
{
  ExpiryBook e;
  e.set(1, at(1000));
  std::vector<OrderId> due;

  e.collectDue(at(999), due);
  EXPECT_TRUE(due.empty());

  e.collectDue(at(1000), due);  // good until T is gone AT T
  EXPECT_EQ(due, (std::vector<OrderId>{1}));

  e.collectDue(at(1001), due);
  EXPECT_EQ(due, (std::vector<OrderId>{1}));
}

TEST(ConductExpiry, TheDueOrdersComeBackIdSorted)
{
  ExpiryBook e;
  for (OrderId id : {40U, 10U, 30U, 20U})
  {
    e.set(id, at(100));
  }
  e.set(50, at(10'000));  // not due
  std::vector<OrderId> due;
  e.collectDue(at(100), due);
  EXPECT_EQ(due, (std::vector<OrderId>{10, 20, 30, 40}));
}

TEST(ConductExpiry, TheBufferIsClearedBeforeEachSweep)
{
  ExpiryBook e;
  std::vector<OrderId> due{99, 98};
  e.collectDue(at(100), due);  // empty book: the caller's leftovers must go
  EXPECT_TRUE(due.empty());

  e.set(1, at(100));
  due.assign({99});
  e.collectDue(at(100), due);
  EXPECT_EQ(due, (std::vector<OrderId>{1}));
}

TEST(ConductExpiry, AnAbsentDeadlineReadsAsZero)
{
  ExpiryBook e;
  EXPECT_EQ(e.expiryOf(1).raw(), 0);
  e.set(1, at(1000));
  EXPECT_EQ(e.expiryOf(1).raw(), 1000);
  e.erase(1);
  EXPECT_EQ(e.expiryOf(1).raw(), 0);
  EXPECT_TRUE(e.empty());
}

// ---- pegs -----------------------------------------------------------------

PegBook::Market market(double bid, double ask)
{
  PegBook::Market m;
  m.hasBid = bid > 0;
  m.bidRaw = m.hasBid ? px(bid).raw() : 0;
  m.hasAsk = ask > 0;
  m.askRaw = m.hasAsk ? px(ask).raw() : 0;
  m.lastRaw = px(50.0).raw();
  m.tickRaw = px(0.01).raw();
  m.minPriceRaw = px(1.0).raw();
  m.maxPriceRaw = px(1000.0).raw();
  return m;
}

TEST(ConductPegs, ABidPegTracksTheBidAndAnAskPegTheAsk)
{
  const auto m = market(99.0, 101.0);
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, 0, m), px(99.0).raw());
  EXPECT_EQ(PegBook::targetRaw(Side::SELL, PegRef::Ask, 0, m), px(101.0).raw());
}

TEST(ConductPegs, TheOffsetIsSignedAndAppliedToTheReference)
{
  const auto m = market(99.0, 101.0);
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, px(0.50).raw(), m), px(99.50).raw());
  EXPECT_EQ(PegBook::targetRaw(Side::SELL, PegRef::Ask, -px(0.50).raw(), m), px(100.50).raw());
}

TEST(ConductPegs, AMidPegSitsBetweenTheTouches)
{
  const auto m = market(99.0, 101.0);
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Mid, 0, m), px(100.0).raw());
}

TEST(ConductPegs, AMissingSideFallsBackToTheOtherAndThenToLast)
{
  PegBook::Market oneSided = market(99.0, 0.0);
  // No ask to track, so both fall back to the bid -- and then the no-cross
  // clamp lifts the sell one tick above it.
  EXPECT_EQ(PegBook::targetRaw(Side::SELL, PegRef::Ask, 0, oneSided), px(99.01).raw());
  EXPECT_EQ(PegBook::targetRaw(Side::SELL, PegRef::Mid, 0, oneSided), px(99.01).raw());
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Ask, 0, oneSided), px(99.0).raw());

  PegBook::Market empty = market(0.0, 0.0);
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, 0, empty), px(50.0).raw());
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Mid, 0, empty), px(50.0).raw());
}

TEST(ConductPegs, TheTargetIsAlignedDownToTheTick)
{
  PegBook::Market m = market(99.0, 101.0);
  m.tickRaw = px(0.25).raw();
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, px(0.10).raw(), m), px(99.0).raw());
}

TEST(ConductPegs, APegNeverCrossesTheOppositeTouch)
{
  const auto m = market(99.0, 101.0);
  // A buy pegged a long way above the bid stops one tick below the ask.
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, px(5.0).raw(), m), px(100.99).raw());
  // And a sell pegged below the ask stops one tick above the bid.
  EXPECT_EQ(PegBook::targetRaw(Side::SELL, PegRef::Ask, -px(5.0).raw(), m), px(99.01).raw());
}

TEST(ConductPegs, TheTargetStaysInsideThePriceBand)
{
  PegBook::Market m = market(99.0, 0.0);
  m.maxPriceRaw = px(99.50).raw();
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, px(5.0).raw(), m), px(99.50).raw());

  m.minPriceRaw = px(98.0).raw();
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, -px(5.0).raw(), m), px(98.0).raw());
}

TEST(ConductPegs, ThePassIsOrderedByOrderId)
{
  PegBook p;
  for (OrderId id : {7U, 3U, 11U, 5U})
  {
    p.set(id, PegBook::Peg{Side::BUY, PegRef::Bid, 0});
  }
  std::vector<OrderId> ids{99};  // leftovers from a prior pass must go
  p.sortedIds(ids);
  EXPECT_EQ(ids, (std::vector<OrderId>{3, 5, 7, 11}));

  p.erase(5);
  EXPECT_EQ(p.find(5), nullptr);
  p.sortedIds(ids);
  EXPECT_EQ(ids, (std::vector<OrderId>{3, 7, 11}));
}

TEST(ConductPegs, TheSnapshotIsWrittenInOrderIdOrderAndRoundTrips)
{
  PegBook p;
  p.set(9, PegBook::Peg{Side::SELL, PegRef::Ask, 7});
  p.set(2, PegBook::Peg{Side::BUY, PegRef::Mid, -3});

  TempJournal tj("conduct-pegs");
  const auto recs = tj.write([&](Journal& j)
                             { p.writeSnapshot(j, 42); });

  ASSERT_EQ(recs.size(), 2U);
  PegBook back;
  std::vector<OrderId> ids;
  for (const auto& c : recs)
  {
    const auto* r = std::get_if<RestorePeg>(&c);
    ASSERT_NE(r, nullptr);
    ids.push_back(r->id);
    back.set(r->id, PegBook::Peg{r->side, r->ref, r->offsetRaw});
  }
  EXPECT_EQ(ids, (std::vector<OrderId>{2, 9}));
  EXPECT_EQ(back.hashInto(0), p.hashInto(0));
}

// ---- market-maker protection ---------------------------------------------

TEST(ConductMmp, AnEngineWithNoConfiguredAccountIsNotArmed)
{
  MmpState m;
  EXPECT_FALSE(m.armed());
  m.add(1, qty(1000.0), at(0));  // ignored: nobody asked for protection
  EXPECT_TRUE(m.breached().empty());

  m.configure(1, qty(10.0), DurationNs{1000});
  EXPECT_TRUE(m.armed());
}

TEST(ConductMmp, TheBreachFiresWhenTheWindowSumReachesTheLimit)
{
  MmpState m;
  m.configure(1, qty(10.0), DurationNs{1000});
  m.add(1, qty(4.0), at(0));
  EXPECT_TRUE(m.breached().empty());
  m.add(1, qty(5.0), at(100));
  EXPECT_TRUE(m.breached().empty());
  m.add(1, qty(1.0), at(200));  // sum == limit
  EXPECT_EQ(m.breached(), (std::vector<uint64_t>{1}));
}

TEST(ConductMmp, AnAccountIsQueuedOnce)
{
  MmpState m;
  m.configure(1, qty(1.0), DurationNs{1000});
  m.add(1, qty(5.0), at(0));
  m.add(1, qty(5.0), at(1));
  EXPECT_EQ(m.breached(), (std::vector<uint64_t>{1}));
}

TEST(ConductMmp, FillsOlderThanTheWindowStopCounting)
{
  MmpState m;
  m.configure(1, qty(10.0), DurationNs{1000});
  m.add(1, qty(9.0), at(0));
  m.add(1, qty(9.0), at(5000));  // the first fill has aged out
  EXPECT_TRUE(m.breached().empty());
}

TEST(ConductMmp, RearmingDropsTheWindowAndItsRunningSum)
{
  MmpState m;
  m.configure(1, qty(10.0), DurationNs{1'000'000});
  m.add(1, qty(10.0), at(0));
  ASSERT_EQ(m.breached(), (std::vector<uint64_t>{1}));

  m.rearm(1);
  m.clearBreached();

  // The maker starts the next window from nothing: the fills it was already
  // punished for must not breach it a second time.
  m.add(1, qty(9.0), at(10));
  EXPECT_TRUE(m.breached().empty());
  m.add(1, qty(1.0), at(20));
  EXPECT_EQ(m.breached(), (std::vector<uint64_t>{1}));
}

TEST(ConductMmp, AnEmptyWindowIsIndistinguishableFromAbsence)
{
  MmpState m;
  m.configure(1, qty(10.0), DurationNs{1000});
  m.add(1, qty(3.0), at(0));
  m.rearm(1);

  MmpState fresh;
  fresh.configure(1, qty(10.0), DurationNs{1000});
  EXPECT_EQ(m.hashInto(0), fresh.hashInto(0));
}

TEST(ConductMmp, TheSnapshotCarriesTheConfigThenTheFillsAndRoundTrips)
{
  MmpState m;
  m.configure(2, qty(10.0), DurationNs{1000});
  m.configure(1, qty(20.0), DurationNs{2000});
  m.add(1, qty(3.0), at(10));
  m.add(1, qty(4.0), at(20));

  TempJournal tj("conduct-mmp");
  const auto recs = tj.write([&](Journal& j)
                             { m.writeSnapshot(j, 42); });

  ASSERT_EQ(recs.size(), 3U);
  const auto* c0 = std::get_if<RestoreMmpCfg>(&recs[0]);
  const auto* c1 = std::get_if<RestoreMmpCfg>(&recs[1]);
  const auto* f0 = std::get_if<RestoreMmpFills>(&recs[2]);
  ASSERT_NE(c0, nullptr);
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(f0, nullptr);
  EXPECT_EQ(c0->account, 1U);  // config first, accounts in key order
  EXPECT_EQ(c1->account, 2U);
  EXPECT_EQ(f0->account, 1U);
  ASSERT_EQ(f0->count, 2U);
  EXPECT_EQ(f0->tsNs[0], 10);  // deque (time) order, exact
  EXPECT_EQ(f0->tsNs[1], 20);

  MmpState back;
  back.restoreCfg(c0->account, c0->qtyLimit, c0->windowNs);
  back.restoreCfg(c1->account, c1->qtyLimit, c1->windowNs);
  ASSERT_TRUE(back.restoreFills(*f0));
  EXPECT_EQ(back.hashInto(0), m.hashInto(0));
}

TEST(ConductMmp, AnOverlongFillBatchProvesTheSnapshotCorrupt)
{
  MmpState m;
  RestoreMmpFills r{};
  r.account = 1;
  r.count = kMmpFillBatch + 1;
  EXPECT_FALSE(m.restoreFills(r));
}

}  // namespace
