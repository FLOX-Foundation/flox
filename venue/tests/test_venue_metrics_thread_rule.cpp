/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Who may read the engine's observability accessors, and what they get.
 *
 * docs/venue/perimeter.md:465-468 tells a deployment to read `lastLookStats()`
 * off the engine and hand it to `prom::render`, which iterates it; MetricsServer
 * runs its sampler on the connection thread that answered the scrape. The
 * containers behind those accessors belong to the matching consumer thread and
 * are written on every hold, every admission-profile change and every order
 * that rests or leaves -- and the accessors hand out a reference straight into
 * them. hasHold()/forEachHold() state a thread rule; these three do not.
 *
 * Two separate things are pinned here, because a fix for one is not a fix for
 * the other:
 *
 *  - a reader on another thread must not race the consumer (the concurrent
 *    test below, run under ThreadSanitizer);
 *  - what the reader is handed must not be live storage that keeps changing
 *    underneath it -- a page rendered from a container the consumer is still
 *    writing into is not a scrape of a moment, it is a scrape of no moment at
 *    all.
 *
 * The concurrent test deliberately grows both maps to their final size BEFORE
 * the reader starts, so the race it reports is the value write and not a
 * rehash: a rehash race also corrupts the reader's iteration, which is a crash
 * rather than a test result.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/prometheus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t kTaker = 900;
constexpr uint64_t kPrinter = 901;
// Accounts that only ever carry an admission profile, so setting one never
// interferes with the order flow the consumer is running.
constexpr uint64_t kProfileBase = 10000;
constexpr size_t kMakers = 16;
constexpr size_t kProfiles = 16;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.lastLookWindowNs = DurationNs{1'000'000'000};
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

// The engine's own sink, called on the thread that submitted the command --
// the consumer thread in a running shard, this test's driver thread here.
struct Cap
{
  std::vector<OutboundEvent> ev;
  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }
  const FillHeld* lastHeld() const
  {
    const FillHeld* last = nullptr;
    for (const auto& e : ev)
    {
      if (const auto* h = std::get_if<FillHeld>(&e))
      {
        last = h;
      }
    }
    return last;
  }
};

struct Driver
{
  Cap cap;
  MatchingEngine<MatchingBook> eng{cfg(), cap.sink()};
  int64_t ts = 0;
  OrderId nextId = 100;

  // A hold measures the move against the last print, so there has to be one.
  void primeReference()
  {
    eng.submit(InboundCommand{limit(nextId++, Side::SELL, 100, 1, kPrinter)}, ++ts);
    eng.submit(InboundCommand{limit(nextId++, Side::BUY, 100, 1, kPrinter)}, ++ts);
  }

  // One full hold cycle for `maker`: it quotes under last look, a taker lifts
  // it, the maker answers. The answer is what writes the maker's row of
  // LastLookStats.
  void episode(uint64_t maker, bool confirm)
  {
    const OrderId makerId = nextId++;
    NewOrder mk = limit(makerId, Side::SELL, 100, 1, maker);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, ++ts);
    const OrderId takerId = nextId++;
    eng.submit(InboundCommand{limit(takerId, Side::BUY, 100, 1, kTaker)}, ++ts);
    const FillHeld* held = cap.lastHeld();
    if (held == nullptr)
    {
      return;
    }
    const uint64_t heldId = held->heldId;
    eng.submit(InboundCommand{LastLookDecision{heldId, SYM, confirm, {}, maker}}, ++ts);

    // A refused hold puts both legs back on the book; left there the next
    // episode matches against them instead of taking a hold of its own.
    for (const auto& [id, account] : {std::pair<OrderId, uint64_t>{makerId, maker},
                                      std::pair<OrderId, uint64_t>{takerId, kTaker}})
    {
      CancelOrder c;
      c.id = id;
      c.symbol = SYM;
      c.accountId = account;
      eng.submit(InboundCommand{c}, ++ts);
    }
    cap.ev.clear();
  }

  // A profile whose fields are never all zero, so setAdmissionProfile updates
  // the row instead of erasing it.
  void setProfile(uint64_t account, uint32_t salt)
  {
    SetAdmissionProfile p{};
    p.symbol = SYM;
    p.account = account;
    p.profile.allowedTypes = 1u | (salt & 0xFEu);
    p.profile.allowedTif = 0xFu;
    p.profile.deny = 0;
    eng.submit(InboundCommand{p}, ++ts);
  }

  // Resting orders come and go, which is what restingOrderCount() counts.
  void churnRestingOrders(uint32_t round)
  {
    const OrderId id = nextId++;
    eng.submit(InboundCommand{limit(id, Side::BUY, 90.0 + (round % 5) * 0.01, 1, kTaker)}, ++ts);
    CancelOrder c;
    c.id = id;
    c.symbol = SYM;
    c.accountId = kTaker;
    eng.submit(InboundCommand{c}, ++ts);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// (1) The thread rule. A /metrics scrape samples the engine from the
// connection thread that answered it, exactly as perimeter.md instructs, while
// the consumer keeps matching.
//
// HOW LONG, AND WHY. A race is a probability, not an event, and the numbers
// below are the ones that turned each flaw from sometimes-visible into
// always-visible on this host, measured by putting the flaw back:
//
//   one pass, one sampler, 2000 rounds   an unlocked sampler: caught 0 of 3
//   12 passes, 4 samplers, 1000 rounds   an unlocked sampler: caught 15 of 15
//                                        a relaxed unlock:    caught 15 of 15
//
// The sampler thread is fresh on every pass, because a thread that has been
// running for a while has already lost every interleaving it was going to
// lose. Four samplers rather than one, because the narrowest flaw here -- an
// unlock that publishes nothing, where the machine happens to publish the
// bytes anyway -- only shows when threads alternate INSIDE the critical
// section, and one reader never alternates with itself.
//
// None of this rests on ThreadSanitizer, which reports these particular races
// on neither the mutated nor the honest tree: the consumer and the sampler
// still meet on other locks, and an ordering through any one of them launders
// the rest. What catches them is the row that does not balance, and that shows
// in an ordinary release build.

namespace
{
constexpr int kConcurrentPasses = 12;
constexpr uint32_t kRoundsPerPass = 1000;
constexpr int kSamplers = 4;
// The admission table is retuned far more often than a hold is answered: one
// row and a bare setter are cheap, so the pass has to be long enough for the
// sampler to land inside a write.
constexpr uint32_t kRetunesPerPass = 6000;
}  // namespace

TEST(VenueMetricsThreadRule, AMetricsThreadMaySampleTheAccessorsWhileOrdersFlow)
{
  Driver d;
  d.primeReference();

  // Grow both maps to their final shape first: every key the concurrent phase
  // touches already exists, so the only concurrent writes are to values.
  for (size_t i = 0; i < kMakers; ++i)
  {
    d.episode(1 + i, /*confirm=*/(i % 2) == 0);
  }
  for (size_t i = 0; i < kProfiles; ++i)
  {
    d.setProfile(kProfileBase + i, static_cast<uint32_t>(i));
  }
  ASSERT_EQ(d.eng.lastLookStats().size(), kMakers);
  ASSERT_EQ(d.eng.admissionProfiles().size(), kProfiles);

  std::atomic<uint64_t> polls{0};
  std::atomic<uint64_t> inconsistent{0};
  std::atomic<uint64_t> sunk{0};

  for (int pass = 0; pass < kConcurrentPasses; ++pass)
  {
    std::atomic<bool> stop{false};
    const uint64_t pollsAtStart = polls.load(std::memory_order_relaxed);

    // What a deployment actually runs: sample the three accessors and fold
    // them into a page. Nothing here writes to the engine.
    const auto sample = [&]
    {
      while (!stop.load(std::memory_order_relaxed))
      {
        uint64_t fold = 0;
        for (const auto& [maker, st] : d.eng.lastLookStats())
        {
          (void)maker;
          // Invariants of one consistent row. Every hold this test takes is
          // answered, and LastLook::record writes `held` and then exactly one
          // of accepted/rejected, so a row at rest always balances. A reader
          // that lands between those two writes sees it not balance -- which
          // is the whole of the finding, visible without a sanitizer.
          if (st.held != st.accepted + st.rejected)
          {
            inconsistent.fetch_add(1, std::memory_order_relaxed);
          }
          if (st.rejectedAdverse + st.rejectedFavourable > st.rejected)
          {
            inconsistent.fetch_add(1, std::memory_order_relaxed);
          }
          fold += st.held + st.accepted + st.rejected;
        }
        for (const auto& [account, profile] : d.eng.admissionProfiles())
        {
          fold += account + profile.allowedTypes + profile.allowedTif + profile.deny;
        }
        fold += d.eng.restingOrderCount();
        sunk.fetch_add(fold, std::memory_order_relaxed);
        polls.fetch_add(1, std::memory_order_relaxed);
      }
    };

    std::vector<std::thread> samplers;
    for (int i = 0; i < kSamplers; ++i)
    {
      samplers.emplace_back(sample);
    }

    // Do not start writing until a sampler is actually sampling, so the
    // threads overlap rather than run one after the other.
    while (polls.load(std::memory_order_relaxed) == pollsAtStart)
    {
      std::this_thread::yield();
    }

    for (uint32_t round = 0; round < kRoundsPerPass; ++round)
    {
      d.episode(1 + (round % kMakers), /*confirm=*/(round % 3) == 0);
      d.setProfile(kProfileBase + (round % kProfiles), round);
      d.churnRestingOrders(round);
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : samplers)
    {
      t.join();
    }
  }

  EXPECT_GT(polls.load(), 0u);
  EXPECT_EQ(inconsistent.load(), 0u)
      << "the /metrics thread read a row the consumer was part-way through writing";
  (void)sunk.load();
}

// ---------------------------------------------------------------------------
// (1b) The admission table on its own.
//
// In the test above both sides of every round also pass through the last-look
// lock, which orders them by accident: a setter that takes no lock of its own
// is hidden behind somebody else's. Here the consumer does NOTHING but retune
// accounts, so the admission table is the only thing shared and the only lock
// that can order it is its own.
//
// And the sampled row is checked, not just folded into a sum. Each profile is
// written as one of two triples whose three fields all differ, and all three
// are set together; a row that comes back mixing one triple's allowedTypes
// with the other's allowedTif was copied part-way through a write. That is the
// same failure as an unbalanced LastLookStats row, in the table that had no
// value check at all.

namespace
{
struct Triple
{
  uint32_t allowedTypes;
  uint32_t allowedTif;
  uint8_t deny;
};

// Never all-zero (that erases the row), and no field value is shared between
// the two, so any mixture of them is recognisable.
constexpr uint64_t kHotAccount = kProfileBase;
constexpr Triple kProfileA{0x000000F0u, 0x0000000Fu, AdmissionDeny::DenyResting};
constexpr Triple kProfileB{0x0F000000u, 0x000000F0u, AdmissionDeny::DenyQuote};

bool isWhole(const AdmissionProfile& p)
{
  const auto matches = [&p](const Triple& t)
  { return p.allowedTypes == t.allowedTypes && p.allowedTif == t.allowedTif && p.deny == t.deny; };
  return matches(kProfileA) || matches(kProfileB);
}
}  // namespace

TEST(VenueMetricsThreadRule, AMetricsThreadMaySampleAdmissionProfilesWhileTheyAreRetuned)
{
  Driver d;

  const auto put = [&d](uint64_t account, const Triple& t)
  {
    SetAdmissionProfile p{};
    p.symbol = SYM;
    p.account = account;
    p.profile.allowedTypes = t.allowedTypes;
    p.profile.allowedTif = t.allowedTif;
    p.profile.deny = t.deny;
    d.eng.submit(InboundCommand{p}, ++d.ts);
  };

  // ONE account, retuned over and over. A table of many rows spreads the
  // writes over many cache lines and the sampler's copy over many reads, and
  // the two stop landing on each other: measured here, sixteen rows never
  // caught a torn row in four million writes and one row caught thousands.
  // One account being retuned while the page is sampled is also the shape of
  // the real thing -- a risk desk tightening one counterparty.
  //
  // The row exists before the sampler starts, so every concurrent write is an
  // assignment into it and never an insert that would rehash under a reader.
  put(kHotAccount, kProfileA);
  ASSERT_EQ(d.eng.admissionProfiles().size(), 1u);

  std::atomic<uint64_t> polls{0};
  std::atomic<uint64_t> torn{0};
  std::atomic<uint64_t> rows{0};

  for (int pass = 0; pass < kConcurrentPasses; ++pass)
  {
    std::atomic<bool> stop{false};
    const uint64_t pollsAtStart = polls.load(std::memory_order_relaxed);

    const auto sample = [&]
    {
      while (!stop.load(std::memory_order_relaxed))
      {
        const auto page = d.eng.admissionProfiles();
        for (const auto& [account, profile] : page)
        {
          (void)account;
          if (!isWhole(profile))
          {
            torn.fetch_add(1, std::memory_order_relaxed);
          }
        }
        rows.fetch_add(page.size(), std::memory_order_relaxed);
        polls.fetch_add(1, std::memory_order_relaxed);
      }
    };

    std::vector<std::thread> samplers;
    for (int i = 0; i < kSamplers; ++i)
    {
      samplers.emplace_back(sample);
    }
    while (polls.load(std::memory_order_relaxed) == pollsAtStart)
    {
      std::this_thread::yield();
    }

    for (uint32_t round = 0; round < kRetunesPerPass; ++round)
    {
      put(kHotAccount, (round % 2) == 0 ? kProfileB : kProfileA);
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : samplers)
    {
      t.join();
    }
  }

  EXPECT_GT(polls.load(), 0u);
  EXPECT_GT(rows.load(), 0u);
  EXPECT_EQ(torn.load(), 0u)
      << "a sampled admission row mixed the two profiles: it was copied while it was "
         "being written";
  // The table is whole and unchanged when the dust settles.
  const auto settled = d.eng.admissionProfiles();
  ASSERT_EQ(settled.size(), 1u);
  EXPECT_TRUE(isWhole(settled.at(kHotAccount)));
}

// ---------------------------------------------------------------------------
// (2) What the reader is handed. A reference into live storage is not a
// sample: it keeps moving while the page is being built, so two series on one
// page can come from two different moments, and nothing the reader does can
// pin it.
//
// The surface these two hold the engine to:
//   std::unordered_map<uint64_t, LastLookStats>    lastLookStats() const;
//   std::unordered_map<uint64_t, AdmissionProfile> admissionProfiles() const;
// By value, and taken under the rule that keeps it coherent -- a copy made
// while the consumer is writing is a snapshot of nothing.

TEST(VenueMetricsThreadRule, TheAccessorsHandOutASnapshotNotLiveStorage)
{
  EXPECT_FALSE(std::is_reference_v<decltype(std::declval<const MatchingEngine<MatchingBook>&>()
                                                .lastLookStats())>)
      << "lastLookStats() has to hand out a snapshot, not a reference into live storage";
  EXPECT_FALSE(std::is_reference_v<decltype(std::declval<const MatchingEngine<MatchingBook>&>()
                                                .admissionProfiles())>)
      << "admissionProfiles() has to hand out a snapshot, not a reference into live storage";

  Driver d;
  d.primeReference();
  d.episode(1, /*confirm=*/true);
  d.setProfile(kProfileBase, 1);

  const auto& stats = d.eng.lastLookStats();
  const auto& profiles = d.eng.admissionProfiles();
  const size_t statsAtSample = stats.size();
  const size_t profilesAtSample = profiles.size();
  const uint64_t heldAtSample = stats.at(1).held;
  ASSERT_EQ(statsAtSample, 1u);
  ASSERT_EQ(profilesAtSample, 1u);

  // The venue keeps trading after the scrape was sampled.
  d.episode(2, /*confirm=*/false);
  d.episode(1, /*confirm=*/false);
  d.setProfile(kProfileBase + 1, 2);

  EXPECT_EQ(stats.size(), statsAtSample)
      << "the sampled last-look stats grew behind the reader";
  EXPECT_EQ(stats.at(1).held, heldAtSample)
      << "a row of the sampled last-look stats changed behind the reader";
  EXPECT_EQ(profiles.size(), profilesAtSample)
      << "the sampled admission profiles grew behind the reader";
}

// ---------------------------------------------------------------------------
// (3) GREEN CONTROL. The numbers themselves. A snapshot must report exactly
// what a reference reported; a fix that hands out an empty or stale copy would
// render a page of zeros, and zeros look exactly like a venue where nothing
// has ever happened (the failure docs/venue/perimeter.md:470-473 names).

TEST(VenueMetricsThreadRule, TheAccessorsStillReportWhatTheEngineDid)
{
  Driver d;
  d.primeReference();
  d.episode(1, /*confirm=*/true);
  d.episode(1, /*confirm=*/false);
  d.episode(2, /*confirm=*/false);
  d.setProfile(kProfileBase, 1);
  d.setProfile(kProfileBase + 1, 2);

  const auto stats = d.eng.lastLookStats();
  ASSERT_EQ(stats.size(), 2u);
  EXPECT_EQ(stats.at(1).held, 2u);
  EXPECT_EQ(stats.at(1).accepted, 1u);
  EXPECT_EQ(stats.at(1).rejected, 1u);
  EXPECT_EQ(stats.at(2).held, 1u);
  EXPECT_EQ(stats.at(2).rejected, 1u);

  const auto profiles = d.eng.admissionProfiles();
  EXPECT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles.at(kProfileBase).allowedTif, 0xFu);

  // And the page built from them carries the same numbers.
  LastLookSample l;
  l.byMaker = d.eng.lastLookStats();
  l.toleranceRejectedHolds = d.eng.toleranceRejectedHolds();
  l.skippedLastLookProRata = d.eng.skippedLastLookProRata();
  const std::string page = prom::render(Metrics{}, Gauges{}, l);
  EXPECT_NE(page.find("fme_last_look_holds_total{maker=\"1\"} 2"), std::string::npos) << page;

  // An empty book after every episode cleaned up after itself.
  const uint64_t resting = d.eng.restingOrderCount();
  d.churnRestingOrders(0);
  EXPECT_EQ(d.eng.restingOrderCount(), resting);
}
