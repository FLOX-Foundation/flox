/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Golden replay: the safety net matching_engine.h is taken apart behind.
 *
 * Every other test in this tree says "the engine did what I expected here".
 * That is worth having and it is not enough to refactor 5395 lines behind,
 * because a refactor that changes behaviour NOBODY WROTE A TEST FOR passes all
 * of them. This test asserts something stronger and dumber: a corpus of
 * scenarios produces exactly the numbers it produced before, byte for byte,
 * and the numbers are written down in the repo rather than recomputed from the
 * code under test.
 *
 * Three numbers per scenario:
 *   state   -- MatchingEngine::stateHash() at the end of the run: the book,
 *              holds, positions, reservations, MMP windows, clOrdId sets.
 *   config  -- MatchingEngine::configHash(): the knobs a snapshot is read back
 *              under. A change here invalidates stored snapshots.
 *   stream  -- every outbound event of the run, in order, through
 *              event_hash.h. This is the half that catches a lost publication:
 *              a TradingStatusChanged that stops being emitted, or an
 *              OrderCanceled an expiry no longer reports, leaves state
 *              identical and the stream different.
 *
 * For the checkpoint scenarios the stream digest also carries the snapshot
 * record trace: for each record written by writeSnapshot, in file order, its
 * wire tag and the loader engine's stateHash AFTER applying it. That is what
 * makes reordering two snapshot records red -- the end state is order-
 * insensitive by construction (it is a sorted digest), the intermediate states
 * are not.
 *
 * Determinism rules for anything added here: no wall clock, no getenv in a
 * scenario, no unordered iteration that reaches an event, no RNG but the
 * seeded xorshift in workload.h. Sequencer time is injected -- every submit
 * carries an explicit tick.
 *
 * When a change to the engine is INTENDED, the table is updated by hand:
 *
 *     FLOX_UPDATE_GOLDEN=1 ./test_venue_golden_replay
 *
 * and the rewritten venue/tests/golden/replay_hashes.txt goes in the same
 * commit as the change, where a reviewer has to look at it. It is never
 * updated by CI and never updated as a way of making the test pass.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/workload.h"
#include "flox/book/ladder_book.h"
#include "support/engine_surface.h"
#include "support/recovery_scenario.h"
#include "support/tmp_path.h"

#include "flox/clearing/fee_schedule.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

#ifndef FLOX_VENUE_GOLDEN_TABLE
#error "FLOX_VENUE_GOLDEN_TABLE must name the golden hash table (set in venue/CMakeLists.txt)"
#endif

namespace
{

constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 900;
constexpr uint64_t kDigestSeed = 1469598103934665603ULL;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
int64_t baseRaw(double v) { return static_cast<int64_t>(amountOf(qty(v))); }
int64_t quoteRaw(double v) { return static_cast<int64_t>(amountOf(Volume::fromDouble(v))); }

// ---- digests --------------------------------------------------------------

constexpr size_t kEventKinds = std::variant_size_v<OutboundEvent>;

// In OutboundEvent's declaration order. The static_assert below is what keeps
// this list honest when an alternative is appended.
constexpr const char* kEventNames[kEventKinds] = {"OrderAccepted",
                                                  "OrderRejected",
                                                  "Trade",
                                                  "OrderExecuted",
                                                  "OrderCanceled",
                                                  "OrderModified",
                                                  "OrderTriggered",
                                                  "FillHeld",
                                                  "FillRejected",
                                                  "MmpTriggered",
                                                  "FeeCharged",
                                                  "Liquidation",
                                                  "BalanceUpdate",
                                                  "TradingStatusChanged",
                                                  "DerivativesUpdated",
                                                  "CancelRejected",
                                                  "PositionAdjusted"};
static_assert(kEventKinds == 17,
              "new OutboundEvent alternative: name it in kEventNames, and make some scenario in "
              "the corpus produce one -- an event no scenario emits is invisible to this test");

// Three paths that share an event with a commoner sibling and would otherwise
// be invisible in the count: deleveraging looks like any other Liquidation, a
// bankruptcy looks like a solvent close, and a maker rebate looks like a fee.
constexpr size_t kProbes = kEventKinds + 3;
constexpr const char* kProbeNames[kProbes] = {kEventNames[0],
                                              kEventNames[1],
                                              kEventNames[2],
                                              kEventNames[3],
                                              kEventNames[4],
                                              kEventNames[5],
                                              kEventNames[6],
                                              kEventNames[7],
                                              kEventNames[8],
                                              kEventNames[9],
                                              kEventNames[10],
                                              kEventNames[11],
                                              kEventNames[12],
                                              kEventNames[13],
                                              kEventNames[14],
                                              kEventNames[15],
                                              kEventNames[16],
                                              "Liquidation(adl)",
                                              "Liquidation(bankrupt)",
                                              "FeeCharged(rebate)"};

using Coverage = std::array<uint64_t, kProbes>;

struct Digest
{
  uint64_t h{kDigestSeed};
  Coverage n{};
  void event(const OutboundEvent& e)
  {
    h = hashEvent(h, e);
    ++n[e.index()];
    if (const auto* l = std::get_if<Liquidation>(&e))
    {
      n[kEventKinds + 0] += l->adl ? 1 : 0;
      n[kEventKinds + 1] += l->bankrupt ? 1 : 0;
    }
    else if (const auto* f = std::get_if<FeeCharged>(&e))
    {
      n[kEventKinds + 2] += f->fee.raw() < 0 ? 1 : 0;
    }
  }
  void note(uint64_t v) { h = mix(h, v); }
};

struct Hashes
{
  uint64_t state{0};
  uint64_t config{0};
  uint64_t stream{0};
  Coverage n{};
};

std::string hex(uint64_t v)
{
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

// ---- the driver -----------------------------------------------------------

// A pristine book instance, sized for the corpus's price band. MatchingBook
// needs no per-symbol config (a std::map has no capacity to reserve); a
// LadderBook's dense ladders and node pool do, and the two facts that decide
// them -- the price band and the tick -- come straight off SymbolConfig,
// which every scenario in this corpus already sets (min/maxPrice, tickSize),
// so there is nothing scenario-specific to plumb through here. maxOrders
// matches test_venue_differential_fuzz.cpp's figure: this corpus never
// exceeds a few thousand resting orders, so 1<<20 is headroom, not a tuned
// number -- the same one already carries a 2M-command fuzz run in CI.
template <class Book>
Book makeBook(const SymbolConfig& c)
{
  if constexpr (std::is_same_v<Book, LadderBook>)
  {
    LadderBook::Config lc;
    lc.basePriceRaw = c.minPrice.raw();
    lc.tickRaw = c.tickSize.raw();
    lc.numLevels = static_cast<int32_t>((c.maxPrice.raw() - c.minPrice.raw()) / lc.tickRaw) + 1;
    lc.maxOrders = 1 << 20;
    return Book{lc};
  }
  else
  {
    return Book{};
  }
}

// One engine plus the digest its events fold into. Not copyable or movable:
// the sink captures `this`, and a moved RunT would keep feeding the old one.
// Templated on the resting-book implementation so the SAME scenario code in
// corpus<Book>() below drives MatchingEngine<MatchingBook> and
// MatchingEngine<LadderBook> alike -- see the contract note above corpus().
template <class Book>
struct RunT
{
  Digest& dig;
  Digest* tap{nullptr};  // optional second digest, for the continuity check
  Ledger led;
  std::vector<OutboundEvent> since;  // events of the last submit
  MatchingEngine<Book> eng;
  int64_t ts{1};

  RunT(const SymbolConfig& c, Digest& d)
      : dig(d), eng(c, [this](const OutboundEvent& e)
                    {
                       dig.event(e);
                       if (tap != nullptr)
                       {
                         tap->event(e);
                       }
                       since.push_back(e); }, makeBook<Book>(c))
  {
  }
  RunT(const RunT&) = delete;
  RunT& operator=(const RunT&) = delete;

  void push(const InboundCommand& cmd)
  {
    since.clear();
    eng.submit(cmd, ts++);
  }
  void tickTo(int64_t at)
  {
    since.clear();
    ts = at + 1;
    eng.tick(at);
  }
  Hashes hashes() const
  {
    return Hashes{eng.stateHash(), eng.configHash(), dig.h, dig.n};
  }
};

template <class Book>
using Setup = std::function<void(RunT<Book>&)>;

// gtest's fixture already has a member named Run, so a TEST body cannot name
// the type unqualified.
using EngineRun = RunT<MatchingBook>;

// Last-look responder. A LastLookDecision names a heldId the engine invents,
// so the answer cannot be generated ahead of time -- the corpus reacts to the
// FillHeld it just saw. One third of the holds are accepted, one third
// rejected, one third left to the window: all three outcomes have to be in the
// stream, or the timeout path is only exercised by the tests that already know
// about it.
struct HoldResponder
{
  workload::Rng rng;
  struct Pending
  {
    uint64_t heldId{};
    uint64_t account{};
    int64_t dueTs{};
    bool accept{};
  };
  std::vector<Pending> queue;

  template <class Book>
  void observe(const RunT<Book>& r, int64_t nowTs)
  {
    for (const OutboundEvent& e : r.since)
    {
      const auto* h = std::get_if<FillHeld>(&e);
      if (h == nullptr)
      {
        continue;
      }
      const uint64_t v = rng.next();
      const uint32_t choice = static_cast<uint32_t>(v % 3);
      if (choice == 2)
      {
        continue;  // never answered: the window decides
      }
      queue.push_back(Pending{h->heldId, h->makerAccount,
                              nowTs + 1 + static_cast<int64_t>((v >> 8) % 5), choice == 0});
    }
  }

  template <class Book>
  void drain(RunT<Book>& r, SymbolId sym)
  {
    for (size_t i = 0; i < queue.size();)
    {
      if (queue[i].dueTs > r.ts)
      {
        ++i;
        continue;
      }
      const Pending p = queue[i];
      queue.erase(queue.begin() + static_cast<long>(i));
      r.push(InboundCommand{LastLookDecision{p.heldId, sym, p.accept, {}, p.account}});
    }
  }
};

template <class Book>
void drive(RunT<Book>& r, const std::vector<InboundCommand>& cmds, HoldResponder* holds, SymbolId sym)
{
  for (const InboundCommand& cmd : cmds)
  {
    if (holds != nullptr)
    {
      holds->drain(r, sym);
    }
    const int64_t at = r.ts;
    r.push(cmd);
    if (holds != nullptr)
    {
      holds->observe(r, at);
    }
  }
}

// ---- symbol configs -------------------------------------------------------

SymbolConfig spotCfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  return c;
}

SymbolConfig perpCfg(bool adl)
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;     // 10% -> 10x
  c.maintenanceMarginBps = 500;  // 5%
  c.autoDeleverage = adl;
  c.maxPositionQty = qty(20);
  c.fundingIntervalNs = DurationNs{8000};
  return c;
}

workload::Params params(uint64_t seed, size_t count, uint32_t accounts = 8)
{
  workload::Params p;
  p.symbol = SYM;
  p.mid = 100.0;
  p.tick = 0.01;
  p.seed = seed;
  p.count = count;
  p.accounts = accounts;
  return p;
}

std::vector<InboundCommand> deposits(uint32_t accounts, double baseAmt, double quoteAmt)
{
  std::vector<InboundCommand> v;
  for (uint32_t a = 1; a <= accounts; ++a)
  {
    if (baseAmt > 0.0)
    {
      v.emplace_back(Deposit{a, BASE, {}, baseRaw(baseAmt), SYM});
    }
    v.emplace_back(Deposit{a, QUOTE, {}, quoteRaw(quoteAmt), SYM});
  }
  return v;
}

std::vector<InboundCommand> concat(std::vector<InboundCommand> a,
                                   const std::vector<InboundCommand>& b)
{
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// ---- plain scenarios ------------------------------------------------------

template <class Book>
Hashes plain(const SymbolConfig& c, const std::vector<InboundCommand>& cmds, const Setup<Book>& setup,
             bool lastLook, int64_t finalTick)
{
  Digest d;
  RunT<Book> r(c, d);
  if (setup)
  {
    setup(r);
  }
  HoldResponder holds;
  drive(r, cmds, lastLook ? &holds : nullptr, c.id);
  if (finalTick > 0)
  {
    r.tickTo(finalTick);
  }
  return r.hashes();
}

// ---- the checkpoint scenario ----------------------------------------------

// Run the stream straight through on one engine; run the SAME stream on a
// second, interrupted halfway by writeSnapshot -> fresh engine ->
// applySnapshotRecord -> the rest of the stream. The two must end in the same
// state and, from the snapshot on, emit the same events. The golden stream
// digest of this scenario carries the snapshot record trace as well, so the
// ORDER writeSnapshot writes its records in is nailed down too.
// `viaClone` runs the checkpoint half through cloneForSnapshot rather than
// through the live engine's own writeSnapshot -- the path production actually
// takes (SequencedShard::doCheckpoint clones off the hot path and serializes
// the clone in the background; see venue/include/flox-venue/sequenced_shard.h).
// The two are not the same thing: a field the clone forgets to copy is
// invisible to a snapshot written straight off the live engine, whose own
// copy of that field was never wrong. Default false leaves every existing
// scenario's bytes, and golden hashes, untouched.
template <class Book>
Hashes checkpointed(const SymbolConfig& c, const std::vector<InboundCommand>& cmds,
                    const Setup<Book>& setup, bool lastLook, const char* pathStem,
                    bool viaClone = false)
{
  const size_t half = cmds.size() / 2;

  Digest contD;
  Digest contTail;
  RunT<Book> cont(c, contD);
  if (setup)
  {
    setup(cont);
  }
  {
    HoldResponder holds;
    for (size_t i = 0; i < cmds.size(); ++i)
    {
      if (i == half)
      {
        cont.tap = &contTail;
      }
      if (lastLook)
      {
        holds.drain(cont, c.id);
      }
      const int64_t at = cont.ts;
      cont.push(cmds[i]);
      if (lastLook)
      {
        holds.observe(cont, at);
      }
    }
  }

  Digest d;
  Digest splitTail;
  Hashes out{};
  {
    RunT<Book> a(c, d);
    if (setup)
    {
      setup(a);
    }
    HoldResponder holds;
    for (size_t i = 0; i < half; ++i)
    {
      if (lastLook)
      {
        holds.drain(a, c.id);
      }
      const int64_t at = a.ts;
      a.push(cmds[i]);
      if (lastLook)
      {
        holds.observe(a, at);
      }
    }

    const std::string path = tmpPath(pathStem, ".snap");
    std::remove(path.c_str());
    {
      Journal out2(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
      if (viaClone)
      {
        auto clone = a.eng.cloneForSnapshot(makeBook<Book>(c));
        clone.engine->writeSnapshot(out2);
      }
      else
      {
        a.eng.writeSnapshot(out2);
      }
      out2.flush();
    }
    const auto records = Journal::loadTimed(path);
    std::remove(path.c_str());

    RunT<Book> b(c, d);
    if (setup)
    {
      setup(b);
    }
    b.ts = a.ts;
    bool allApplied = true;
    for (const auto& [rts, cmd] : records)
    {
      allApplied = b.eng.applySnapshotRecord(cmd, rts) && allApplied;
      // Tag and the state it left behind: swap two records and the trace
      // differs even though the end state does not.
      d.note(wireTagOf(cmd));
      d.note(b.eng.stateHash());
    }
    EXPECT_TRUE(allApplied) << "snapshot record refused by the loader: " << pathStem;
    EXPECT_GE(records.size(), 2u) << "snapshot is empty: " << pathStem;

    b.tap = &splitTail;
    // The SAME responder carries on across the restart, with its queue of
    // undecided holds intact. A snapshot preserves heldIds, so a decision the
    // maker had already made on a hold opened before the checkpoint still
    // names a hold the restored engine knows -- and the two runs stay on
    // identical input, which is the only way the comparison below means
    // anything. Resetting it here would compare two different histories.
    for (size_t i = half; i < cmds.size(); ++i)
    {
      if (lastLook)
      {
        holds.drain(b, c.id);
      }
      const int64_t at = b.ts;
      b.push(cmds[i]);
      if (lastLook)
      {
        holds.observe(b, at);
      }
    }

    // The point of the scenario: a checkpoint in the middle of the stream is
    // invisible from the outside.
    EXPECT_EQ(b.eng.stateHash(), cont.eng.stateHash())
        << "checkpointed run diverged in state: " << pathStem;
    EXPECT_EQ(b.eng.configHash(), cont.eng.configHash())
        << "checkpointed run diverged in config: " << pathStem;
    EXPECT_EQ(splitTail.h, contTail.h)
        << "checkpointed run diverged in the post-snapshot event stream: " << pathStem;
    out = b.hashes();
  }
  return out;
}

// ---- the corpus -----------------------------------------------------------

struct Scenario
{
  const char* name;
  std::function<Hashes()> run;
};

// Templated on the resting-book implementation. Every scenario below reads
// or writes only through RunT<Book>/MatchingEngine<Book>, never MatchingBook
// or LadderBook by name, so instantiating corpus<LadderBook>() drives the
// IDENTICAL command streams through the O(1) ladder book instead of the
// std::map reference book.
//
// Contract, worked out from matching_engine.h rather than assumed: stateHash
// and configHash (venue/include/flox-venue/engine/checkpoint.inl) fold in
// cfg_ (the SymbolConfig every scenario already supplies) and the book's
// content through Book::forEachOrder's canonical traversal (price levels
// best-first, FIFO within each level) -- never a book's own internal
// representation (LadderBook::Config's base/tick/levels/capacity are absent
// from both hashes). event_hash.h hashes only OutboundEvent fields, which
// are engine-level, not book-level. So for the SAME SymbolConfig and the
// SAME command stream, MatchingEngine<MatchingBook> and
// MatchingEngine<LadderBook> are contractually required to produce
// byte-identical state, config AND stream hashes -- provided the LadderBook
// is provisioned wide and deep enough to never silently drop an order
// (out-of-band price, exhausted node pool), which makeBook<Book>() above
// guarantees for this corpus. test_venue_differential_fuzz.cpp already
// proves the event-stream half of this over a random 200k-command mix; the
// TWO calls to corpus<...>() in CorpusMatchesTheTable below reuse the SAME
// hand-picked corpus (not a random stream) and check LadderBook against
// exactly the recorded MatchingBook numbers -- ONE table, not a second
// `replay_hashes_ladder.txt`, because the contract says there is nothing for
// a second table to record that isn't already a divergence. See
// docs/venue/verification.md ("Golden replay, on both books").
//
// A LadderBook mutation at either of its two real-fill points (fillBest,
// consumeById -- T058's coverage-gap note) changes RestingOrder::cumQty or
// leaves on the LadderBook side only, so it reddens the LadderBook pass here
// while the MatchingBook pass (and every existing unit test) stays green.
template <class Book>
std::vector<Scenario> corpus()
{
  using Run = RunT<Book>;
  std::vector<Scenario> s;

  // The one command stream the journal/recovery tests are built on, driven
  // through the engine directly. It is the closest thing this tree has to a
  // journal fixture: there is no .bin in the repo because the recovery tests
  // generate their journals from this generator at run time.
  s.push_back({"journal_recovery_scenario",
               []
               {
                 return plain<Book>(test::scenarioConfig(), test::scenarioCommands(), [](Run& r)
                                    { r.eng.setLedger(&r.led, test::kScenarioVenueAccount); }, false, 0);
               }});

  // The same stream, but written to a real journal and replayed out of it --
  // the engine must not be able to tell the file apart from the live stream.
  s.push_back({"journal_recovery_replayed",
               []
               {
                 const std::string path = tmpPath("golden_journal", ".bin");
                 std::remove(path.c_str());
                 {
                   Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
                   int64_t ts = 1;
                   for (const auto& cmd : test::scenarioCommands())
                   {
                     out.append(cmd, ts++);
                   }
                   out.flush();
                 }
                 const auto records = Journal::loadTimed(path);
                 std::remove(path.c_str());

                 Digest d;
                 Run r(test::scenarioConfig(), d);
                 r.eng.setLedger(&r.led, test::kScenarioVenueAccount);
                 for (const auto& [ts, cmd] : records)
                 {
                   r.since.clear();
                   r.eng.submit(cmd, ts);  // the SAME timestamp the journal holds
                 }
                 return r.hashes();
               }});

  // Mixed order-level flow with no clearing: limits, markets, stop-markets,
  // IOC, post-only, icebergs, GTD, OCO, pegs, cancels and modifies.
  s.push_back({"mixed_flow_no_ledger",
               []
               {
                 return plain<Book>(spotCfg(), workload::mixedFlow(params(0xC0FFEE123456789ULL, 20000)),
                                    nullptr, false, 3'000'000);
               }});

  // The same flow with a ledger and a fee schedule bound: settlement,
  // reservations, BalanceUpdate and FeeCharged join the stream.
  s.push_back({"mixed_flow_cleared",
               []
               {
                 return plain<Book>(
                     spotCfg(),
                     concat(deposits(8, 100000.0, 10'000'000.0),
                            workload::mixedFlow(params(0xC0FFEE123456789ULL, 20000))),
                     [](Run& r)
                     {
                       flox::FeeSchedule fs;
                       fs.addTier(0.0, /*makerBps*/ -1.0, /*takerBps*/ 2.0);
                       r.eng.setFeeSchedule(fs);
                       r.eng.setLedger(&r.led, VENUE_ACCT);
                     },
                     false, 3'000'000);
               }});

  // GTD expiry driven to completion: the stream has to carry an OrderCanceled
  // per expired order, and the sweep is what produces them.
  s.push_back({"gtd_expiry_sweep",
               []
               {
                 SymbolConfig c = spotCfg();
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 for (const auto& dep : deposits(8, 100000.0, 10'000'000.0))
                 {
                   r.push(dep);
                 }
                 const auto cmds = workload::mixedFlow(params(0x9E3779B97F4A7C15ULL, 6000));
                 // Ticks partway through so expiry fires mid-stream rather than
                 // all at once at the end.
                 for (size_t i = 0; i < cmds.size(); ++i)
                 {
                   r.push(cmds[i]);
                   if (i % 1000 == 999)
                   {
                     r.tickTo(r.ts + 250'000);
                   }
                 }
                 r.tickTo(4'000'000);
                 return r.hashes();
               }});

  // Last look: holds opened by maker quotes and maker limits, answered
  // accept / reject / not at all, with the window closing the third case.
  s.push_back({"last_look_accept_reject_timeout",
               []
               {
                 SymbolConfig c = spotCfg();
                 c.lastLookWindowNs = DurationNs{40};
                 c.lastLookAcceptOnTimeout = false;
                 return plain<Book>(c, concat(deposits(6, 100000.0, 10'000'000.0), workload::makerFlow(params(0xA11CE5EEDULL, 8000, 6))), [](Run& r)
                                    { r.eng.setLedger(&r.led, VENUE_ACCT); }, true, 0);
               }});

  // The venue-side tolerance band and accept-on-timeout: the other two
  // settings of the same feature, which reject and accept holds the maker
  // never answered for.
  s.push_back({"last_look_tolerance_accept_on_timeout",
               []
               {
                 SymbolConfig c = spotCfg();
                 c.lastLookWindowNs = DurationNs{25};
                 c.lastLookAcceptOnTimeout = true;
                 c.lastLookToleranceRaw = px(0.05).raw();
                 return plain<Book>(c, concat(deposits(6, 100000.0, 10'000'000.0), workload::makerFlow(params(0xBEEF1234ULL, 8000, 6))), [](Run& r)
                                    { r.eng.setLedger(&r.led, VENUE_ACCT); }, true, 0);
               }});

  // A last-look decision from an account that does not own the held maker
  // order. engine/last_look.h::onDecision must refuse it with
  // RejectReason::NotOrderOwner rather than let a stranger settle -- or
  // kill -- someone else's hold. last_look_accept_reject_timeout and
  // last_look_tolerance_accept_on_timeout answer every hold through
  // HoldResponder, which always decides as the recorded makerAccount, so the
  // wrong-account path never reaches the wire anywhere else in this corpus.
  // heldId 1 is exactly the one pair created below -- the first and only
  // hold this engine ever opens -- so the id does not have to be read back
  // off the stream the way HoldResponder does it.
  s.push_back({"last_look_wrong_account_decision",
               []
               {
                 SymbolConfig c = spotCfg();
                 c.lastLookWindowNs = DurationNs{1'000'000};
                 c.lastLookAcceptOnTimeout = false;
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 r.push(InboundCommand{Deposit{2, BASE, {}, baseRaw(100.0), SYM}});
                 r.push(InboundCommand{Deposit{1, QUOTE, {}, quoteRaw(100000.0), SYM}});

                 NewOrder maker;
                 maker.id = 1;
                 maker.symbol = SYM;
                 maker.side = Side::SELL;
                 maker.type = OrderType::LIMIT;
                 maker.price = px(100.0);
                 maker.quantity = qty(5.0);
                 maker.accountId = 2;
                 maker.lastLook = true;
                 r.push(InboundCommand{maker});

                 NewOrder taker;
                 taker.id = 2;
                 taker.symbol = SYM;
                 taker.side = Side::BUY;
                 taker.type = OrderType::LIMIT;
                 taker.price = px(100.0);
                 taker.quantity = qty(5.0);
                 taker.accountId = 1;
                 r.push(InboundCommand{taker});

                 // account 3 never touched this hold: refused as NotOrderOwner,
                 // and the hold stays open.
                 r.push(InboundCommand{LastLookDecision{1, SYM, true, {}, 3}});
                 // the real maker still gets to decide afterwards.
                 r.push(InboundCommand{LastLookDecision{1, SYM, true, {}, 2}});
                 return r.hashes();
               }});

  // MM protection and the mass-cancel / quote-replace path.
  s.push_back({"mmp_quote_mass_cancel",
               []
               {
                 return plain<Book>(spotCfg(), concat(deposits(6, 100000.0, 10'000'000.0), workload::makerFlow(params(0x5EED0001ULL, 8000, 6))), [](Run& r)
                                    {
                                r.eng.setLedger(&r.led, VENUE_ACCT);
                                for (uint64_t a = 1; a <= 6; ++a)
                                {
                                  r.eng.setMmp(a, qty(12.0), DurationNs{500});
                                } }, false, 0);
               }});

  // Mass-cancel over resting orders that carry a client order id.
  // publishCanceled already forwards ro->clientOrderId at every call site
  // (session.inl, publications.inl), but event_hash.h only folds
  // OrderCanceled::clientOrderId into the stream digest when it is non-zero
  // (kept for backward compatibility with events recorded before the field
  // existed) -- and no generator in this corpus ever gives a resting order a
  // non-zero clientOrderId (workload.h never sets NewOrder::clientOrderId),
  // so every mass-cancel elsewhere in the corpus -- mmp_quote_mass_cancel and
  // whichever mixedFlow/makerFlow runs reach MassCancel -- cancels orders
  // whose clientOrderId was always 0. A publishCanceled that silently
  // dropped the field on the way out would still hash the same. Two orders
  // from one account carry distinct client order ids; a third, from a
  // second account, carries none, as a control that the zero case still
  // hashes as before.
  s.push_back({"mass_cancel_named_orders",
               []
               {
                 SymbolConfig c = spotCfg();
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 r.push(InboundCommand{Deposit{5, BASE, {}, baseRaw(50.0), SYM}});
                 r.push(InboundCommand{Deposit{6, BASE, {}, baseRaw(50.0), SYM}});

                 NewOrder o1;
                 o1.id = 1;
                 o1.symbol = SYM;
                 o1.side = Side::SELL;
                 o1.type = OrderType::LIMIT;
                 o1.price = px(101.0);
                 o1.quantity = qty(2.0);
                 o1.accountId = 5;
                 o1.clientOrderId = 7001;
                 r.push(InboundCommand{o1});

                 NewOrder o2 = o1;
                 o2.id = 2;
                 o2.price = px(102.0);
                 o2.clientOrderId = 7002;
                 r.push(InboundCommand{o2});

                 NewOrder o3 = o1;
                 o3.id = 3;
                 o3.price = px(103.0);
                 o3.accountId = 6;
                 o3.clientOrderId = 0;
                 r.push(InboundCommand{o3});

                 r.push(InboundCommand{MassCancel{5, SYM}});
                 r.push(InboundCommand{MassCancel{6, SYM}});
                 return r.hashes();
               }});

  // STPMode::CancelBoth in CONTINUOUS matching. workload.h's makerFlow only
  // ever generates None/CancelOldest/CancelNewest/Decrement (see its
  // stpModes array), so no fuzzed scenario in this corpus ever sends
  // CancelBoth into Matcher::applySelfTradePrevention -- found by mutating
  // that branch to StpOutcome::NotApplicable, which left CorpusMatchesTheTable
  // green. One account rests a SELL, then crosses itself with a BUY carrying
  // stp=CancelBoth: both legs must be canceled and nothing may print.
  s.push_back({"stp_cancel_both_continuous",
               []
               {
                 SymbolConfig c = spotCfg();
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 r.push(InboundCommand{Deposit{10, BASE, {}, baseRaw(50.0), SYM}});
                 r.push(InboundCommand{Deposit{10, QUOTE, {}, quoteRaw(100000.0), SYM}});

                 NewOrder maker;
                 maker.id = 1;
                 maker.symbol = SYM;
                 maker.side = Side::SELL;
                 maker.type = OrderType::LIMIT;
                 maker.price = px(100.0);
                 maker.quantity = qty(5.0);
                 maker.accountId = 10;
                 r.push(InboundCommand{maker});

                 NewOrder taker;
                 taker.id = 2;
                 taker.symbol = SYM;
                 taker.side = Side::BUY;
                 taker.type = OrderType::LIMIT;
                 taker.price = px(100.0);
                 taker.quantity = qty(5.0);
                 taker.accountId = 10;
                 taker.stp = STPMode::CancelBoth;
                 r.push(InboundCommand{taker});
                 return r.hashes();
               }});

  // Self-trade prevention across a firm -- two accounts in one group are one
  // trader, and the flow carries all four modes -- plus admission profiles,
  // which decide what a given account is allowed to send at all.
  s.push_back({"stp_groups_and_admission",
               []
               {
                 return plain<Book>(
                     spotCfg(),
                     concat(deposits(6, 100000.0, 10'000'000.0),
                            workload::makerFlow(params(0x57500042ULL, 8000, 6))),
                     [](Run& r)
                     {
                       r.eng.setLedger(&r.led, VENUE_ACCT);
                       r.eng.setStpGroup(1, 900);
                       r.eng.setStpGroup(2, 900);
                       r.eng.setStpGroup(3, 901);
                       r.eng.setStpGroup(4, 901);
                       AdmissionProfile takerOnly{};
                       takerOnly.deny = AdmissionDeny::DenyResting | AdmissionDeny::DenyQuote;
                       r.eng.setAdmissionProfile(5, takerOnly);
                       AdmissionProfile noAmend{};
                       noAmend.deny = AdmissionDeny::DenyAmend | AdmissionDeny::DenyCancel;
                       r.eng.setAdmissionProfile(6, noAmend);
                     },
                     false, 0);
               }});

  // Self-trade prevention across a firm group during an AUCTION uncross.
  // Continuous matching reads the group scope off the matcher's own copy
  // (Matcher::stpScope, used by applySelfTradePrevention); an auction has no
  // aggressor and instead reads it back through
  // MatchingEngine::stpScope -> StpState::scopeOf (engine/session.inl's
  // uncross loop). Two different call paths onto the same rule -- a scopeOf
  // that quietly falls back to per-account scope leaves continuous STP
  // unaffected and breaks only the uncross, which none of the other STP
  // scenarios in this corpus reaches: stp_groups_and_admission never calls
  // BeginPreOpen, and auction_preopen_continuous never registers a group.
  // Accounts 201 and 202 are one firm (group 700) and cross at 100.00 with
  // CancelBoth -- the pair must never print. Accounts 203 and 204 cross at
  // the same price with no group and no STP, as a control: the uncross
  // itself still has to trade when self-trade prevention does not engage.
  s.push_back({"stp_group_auction_uncross",
               []
               {
                 SymbolConfig c = spotCfg();
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 r.eng.setStpGroup(201, 700);
                 r.eng.setStpGroup(202, 700);
                 r.push(InboundCommand{Deposit{201, BASE, {}, baseRaw(50.0), SYM}});
                 r.push(InboundCommand{Deposit{202, QUOTE, {}, quoteRaw(100000.0), SYM}});
                 r.push(InboundCommand{Deposit{203, BASE, {}, baseRaw(50.0), SYM}});
                 r.push(InboundCommand{Deposit{204, QUOTE, {}, quoteRaw(100000.0), SYM}});
                 r.push(InboundCommand{AdminCmd{SYM, AdminAction::BeginPreOpen}});

                 NewOrder groupSell;
                 groupSell.id = 1;
                 groupSell.symbol = SYM;
                 groupSell.side = Side::SELL;
                 groupSell.type = OrderType::LIMIT;
                 groupSell.price = px(100.0);
                 groupSell.quantity = qty(5.0);
                 groupSell.accountId = 201;
                 groupSell.stp = STPMode::CancelBoth;
                 r.push(InboundCommand{groupSell});

                 NewOrder groupBuy = groupSell;
                 groupBuy.id = 2;
                 groupBuy.side = Side::BUY;
                 groupBuy.accountId = 202;
                 r.push(InboundCommand{groupBuy});

                 NewOrder ctrlSell = groupSell;
                 ctrlSell.id = 3;
                 ctrlSell.accountId = 203;
                 ctrlSell.stp = STPMode::None;
                 r.push(InboundCommand{ctrlSell});

                 NewOrder ctrlBuy = groupBuy;
                 ctrlBuy.id = 4;
                 ctrlBuy.accountId = 204;
                 ctrlBuy.stp = STPMode::None;
                 r.push(InboundCommand{ctrlBuy});

                 r.push(InboundCommand{AdminCmd{SYM, AdminAction::OpenContinuous}});
                 return r.hashes();
               }});

  // Pre-open accumulation, uncross, continuous, re-opening auction, uncross
  // again. A crossed book is legal in pre-open, so the uncross itself is the
  // only thing that prints.
  s.push_back({"auction_preopen_continuous",
               []
               {
                 SymbolConfig c = spotCfg();
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 for (const auto& dep : deposits(8, 100000.0, 10'000'000.0))
                 {
                   r.push(dep);
                 }
                 const auto cmds = workload::mixedFlow(params(0xA0C71044ULL, 6000));
                 r.push(InboundCommand{AdminCmd{SYM, AdminAction::BeginPreOpen}});
                 for (size_t i = 0; i < cmds.size(); ++i)
                 {
                   r.push(cmds[i]);
                   if (i == 1500)
                   {
                     r.push(InboundCommand{AdminCmd{SYM, AdminAction::OpenContinuous}});
                   }
                   if (i == 3000)
                   {
                     r.push(InboundCommand{AdminCmd{SYM, AdminAction::ResumeAuction}});
                   }
                   if (i == 4500)
                   {
                     r.push(InboundCommand{AdminCmd{SYM, AdminAction::OpenContinuous}});
                   }
                 }
                 return r.hashes();
               }});

  // T058 coverage gap: an auction uncross fills a resting order PARTIALLY
  // (MatchingBook::consumeById, engine/session.inl's uncross loop -- a
  // different real-fill mutation point than the plain FIFO fillFront every
  // other scenario here exercises), and the residual is canceled afterward.
  // Without this, a mutation that deletes RestingOrder::cumQty's increment
  // inside consumeById passes every test in this corpus silently: nothing
  // else here ever reports a cancel on an order that was partially filled
  // through an uncross specifically, only through continuous FIFO matching.
  // Same four orders/clearing price as the auction unit tests (BUY 101x5,
  // SELL 99x5, BUY 100x3, SELL 100x2 -> uncrosses at 100 for 7 units; order 3
  // is left resting with 1 of its 3), then the residual is explicitly
  // canceled so the leftover's real cumQty (2) reaches the stream.
  s.push_back({"auction_uncross_residual_cancel",
               []
               {
                 SymbolConfig c = spotCfg();
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 for (const auto& dep : deposits(4, 100000.0, 10'000'000.0))
                 {
                   r.push(dep);
                 }
                 r.push(InboundCommand{AdminCmd{SYM, AdminAction::BeginPreOpen}});

                 NewOrder o1;
                 o1.id = 1;
                 o1.symbol = SYM;
                 o1.side = Side::BUY;
                 o1.type = OrderType::LIMIT;
                 o1.price = px(101.0);
                 o1.quantity = qty(5.0);
                 o1.accountId = 1;
                 r.push(InboundCommand{o1});

                 NewOrder o2 = o1;
                 o2.id = 2;
                 o2.side = Side::SELL;
                 o2.price = px(99.0);
                 o2.accountId = 2;
                 r.push(InboundCommand{o2});

                 NewOrder o3 = o1;
                 o3.id = 3;
                 o3.price = px(100.0);
                 o3.quantity = qty(3.0);
                 o3.accountId = 3;
                 r.push(InboundCommand{o3});

                 NewOrder o4 = o2;
                 o4.id = 4;
                 o4.price = px(100.0);
                 o4.quantity = qty(2.0);
                 o4.accountId = 4;
                 r.push(InboundCommand{o4});

                 r.push(InboundCommand{AdminCmd{SYM, AdminAction::OpenContinuous}});  // uncross: order 3 left with 1
                 r.push(InboundCommand{CancelOrder{3, SYM, {}, 3}});
                 return r.hashes();
               }});

  // Session and status transitions: halt, resume, close, reopen, delist,
  // relist, and an emergency halt-and-cancel. Every one of them publishes a
  // TradingStatusChanged, which is what the "drop a publication" mutation
  // takes away.
  s.push_back({"session_status_transitions",
               []
               {
                 SymbolConfig c = spotCfg();
                 // Tight enough to be breached by the flow's own prints. A
                 // band wider than the stream ever moves is a setting the
                 // corpus carries and never reaches: the LULD pause, its
                 // timed expiry and the status it publishes would all go
                 // unwatched.
                 c.luldBps = 5;
                 c.luldHaltNs = DurationNs{50};
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 for (const auto& dep : deposits(8, 100000.0, 10'000'000.0))
                 {
                   r.push(dep);
                 }
                 const auto cmds = workload::mixedFlow(params(0x5E5510115ULL, 6000));
                 const AdminAction script[] = {
                     AdminAction::Halt, AdminAction::Resume,
                     AdminAction::CloseSession, AdminAction::OpenSession,
                     AdminAction::Delist, AdminAction::Relist,
                     AdminAction::HaltAndCancelAll, AdminAction::Resume};
                 size_t nextAdmin = 0;
                 for (size_t i = 0; i < cmds.size(); ++i)
                 {
                   r.push(cmds[i]);
                   if (i % 700 == 699 && nextAdmin < std::size(script))
                   {
                     r.push(InboundCommand{AdminCmd{SYM, script[nextAdmin++]}});
                   }
                 }
                 return r.hashes();
               }});

  // Perp: margin, mark moves, funding settlements, maintenance-margin
  // liquidations. No ADL: a bankruptcy deficit goes to the insurance fund.
  s.push_back({"perp_funding_liquidation",
               []
               {
                 return plain<Book>(perpCfg(/*adl*/ false), concat(deposits(8, 0.0, 200000.0), workload::perpFlow(params(0x9DEADBEEFULL, 15000))), [](Run& r)
                                    {
                                r.eng.setLedger(&r.led, VENUE_ACCT);
                                r.eng.setFundingSchedule(DurationNs{8000}, SeqNanos::fromRaw(8000)); }, false, 0);
               }});

  // The same flow with auto-deleveraging on, a seeded insurance fund, thin
  // collateral and a mark that walks twice as far. Thin collateral is the
  // point: a well-funded account is liquidated solvently and the insurance
  // fund never pays, so neither the bankruptcy branch nor deleveraging is
  // reached -- the corpus has to contain an account that cannot cover its own
  // loss, or the ADL path is not watched at all.
  s.push_back({"perp_adl_bankruptcy",
               []
               {
                 workload::Params p = params(0xAD155EEDULL, 15000);
                 p.markSpanTicks = 4000;  // +/-40 on a mid of 100
                 return plain<Book>(perpCfg(/*adl*/ true), concat(concat(deposits(8, 0.0, 600.0), {InboundCommand{Deposit{VENUE_ACCT, QUOTE, {}, quoteRaw(500000.0), SYM}}}), workload::perpFlow(p)), [](Run& r)
                                    {
                                r.eng.setLedger(&r.led, VENUE_ACCT);
                                r.eng.setFundingSchedule(DurationNs{8000}, SeqNanos::fromRaw(8000)); }, false, 0);
               }});

  // The same waterfall, written by hand rather than found by a seed. The fuzz
  // above reaches bankruptcy and deleveraging a handful of times out of 15,000
  // commands, which is enough to see them and not enough to rely on: a tweak
  // to a generator could stop reaching them, and the corpus would go quiet
  // about the ADL path without anything turning red. This scenario reaches it
  // on purpose, in six commands, and its arithmetic is legible:
  //   acct 1 deposits 250 and buys 20 @ 100 -- IM 200, wallet 50 left
  //   acct 2 is the other side, short 20 @ 100, well funded
  //   the mark drops to 60: acct 1 owes 800 against 250 of collateral
  //     -> liquidated bankrupt, deficit 550
  //   acct 2 is 800 up on the same move -> deleveraged, 550 of the gain
  //     clawed back before the insurance fund is touched
  s.push_back({"perp_adl_scripted",
               []
               {
                 SymbolConfig c = perpCfg(/*adl*/ true);
                 c.maxPositionQty = Quantity{};  // uncapped: the sizes are chosen here
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 r.push(InboundCommand{Deposit{1, QUOTE, {}, quoteRaw(250.0), SYM}});
                 r.push(InboundCommand{Deposit{2, QUOTE, {}, quoteRaw(50000.0), SYM}});
                 r.push(InboundCommand{Deposit{VENUE_ACCT, QUOTE, {}, quoteRaw(100.0), SYM}});

                 NewOrder maker;
                 maker.id = 1;
                 maker.symbol = SYM;
                 maker.side = Side::SELL;
                 maker.type = OrderType::LIMIT;
                 maker.price = px(100.0);
                 maker.quantity = qty(20.0);
                 maker.accountId = 2;
                 r.push(InboundCommand{maker});

                 NewOrder taker = maker;
                 taker.id = 2;
                 taker.side = Side::BUY;
                 taker.accountId = 1;
                 r.push(InboundCommand{taker});

                 r.push(InboundCommand{SetMark{SYM, {}, px(100.0)}});
                 r.push(InboundCommand{SetMark{SYM, {}, px(60.0)}});
                 return r.hashes();
               }});

  // Checkpoint in the middle of a cleared spot stream.
  s.push_back({"snapshot_midstream_spot",
               []
               {
                 return checkpointed<Book>(
                     spotCfg(),
                     concat(deposits(8, 100000.0, 10'000'000.0),
                            workload::mixedFlow(params(0x51A95407ULL, 8000))),
                     [](Run& r)
                     {
                       flox::FeeSchedule fs;
                       fs.addTier(0.0, -1.0, 2.0);
                       r.eng.setFeeSchedule(fs);
                       r.eng.setLedger(&r.led, VENUE_ACCT);
                     },
                     false, "golden_snap_spot", /*viaClone*/ true);
               }});

  // Checkpoint with open holds, MMP windows and STP groups in flight: the
  // parts of the snapshot that are engine state rather than book state.
  s.push_back({"snapshot_midstream_lastlook",
               []
               {
                 SymbolConfig c = spotCfg();
                 c.lastLookWindowNs = DurationNs{40};
                 c.clOrdIdWindowNs = 2000;
                 return checkpointed<Book>(
                     c,
                     concat(deposits(6, 100000.0, 10'000'000.0),
                            workload::makerFlow(params(0x11AA5EEDULL, 6000, 6))),
                     [](Run& r)
                     {
                       r.eng.setLedger(&r.led, VENUE_ACCT);
                       for (uint64_t a = 1; a <= 6; ++a)
                       {
                         r.eng.setMmp(a, qty(12.0), DurationNs{500});
                       }
                       r.eng.setStpGroup(1, 900);
                       r.eng.setStpGroup(2, 900);
                     },
                     true, "golden_snap_lastlook", /*viaClone*/ true);
               }});

  // Checkpoint with open perp positions, posted margin and a funding
  // calendar: RestorePosition, RestoreReservation and RestoreFunding.
  s.push_back({"snapshot_midstream_perp",
               []
               {
                 return checkpointed<Book>(
                     perpCfg(/*adl*/ false),
                     concat(deposits(8, 0.0, 200000.0),
                            workload::perpFlow(params(0x9E97AA51ULL, 8000))),
                     [](Run& r)
                     {
                       r.eng.setLedger(&r.led, VENUE_ACCT);
                       r.eng.setFundingSchedule(DurationNs{8000}, SeqNanos::fromRaw(8000));
                     },
                     false, "golden_snap_perp", /*viaClone*/ true);
               }});

  // Checkpoint taken while the instrument is delisted. Delist lands last in
  // the pre-checkpoint half; a probe order opens the post-restore half, so it
  // is the very first thing the restored engine sees. `viaClone` routes the
  // checkpoint through cloneForSnapshot -- the path production actually
  // takes -- rather than the live engine's own writeSnapshot: a checkpoint
  // clone that forgets a flag is invisible to the plain path but not to this
  // one. checkpointed()'s own EXPECT_EQ(b.eng.stateHash(), cont.eng.stateHash())
  // already catches a clone that comes back listed, since the continuous run
  // rejects the probe order and a mis-restored split run would accept it.
  s.push_back({"snapshot_midstream_delisted",
               []
               {
                 SymbolConfig c = spotCfg();
                 std::vector<InboundCommand> cmds;
                 cmds.emplace_back(Deposit{1, BASE, {}, baseRaw(100.0), SYM});
                 cmds.emplace_back(Deposit{1, QUOTE, {}, quoteRaw(100000.0), SYM});
                 cmds.emplace_back(Deposit{2, QUOTE, {}, quoteRaw(100000.0), SYM});
                 cmds.emplace_back(InboundCommand{AdminCmd{SYM, AdminAction::Delist}});
                 NewOrder probe;
                 probe.id = 501;
                 probe.symbol = SYM;
                 probe.side = Side::BUY;
                 probe.type = OrderType::LIMIT;
                 probe.price = px(100.0);
                 probe.quantity = qty(1.0);
                 probe.accountId = 2;
                 cmds.emplace_back(probe);
                 NewOrder probe2 = probe;
                 probe2.id = 502;
                 cmds.emplace_back(probe2);
                 cmds.emplace_back(InboundCommand{CancelOrder{999, SYM, {}, 0}});
                 cmds.emplace_back(InboundCommand{Deposit{2, BASE, {}, baseRaw(1.0), SYM}});
                 return checkpointed<Book>(
                     c, cmds, [](Run& r)
                     { r.eng.setLedger(&r.led, VENUE_ACCT); }, false,
                     "golden_snap_delisted", /*viaClone*/ true);
               }});

  // Checkpoint taken with two open holds whose legs were NAMED by their
  // submitters. RestoreHeld carries both client order ids and stateHash folds
  // a non-zero one in, so a restore that drops them cannot even load the file:
  // SnapshotEnd refuses it and checkpointed()'s own allApplied check goes red.
  // Past that, the reports the two decisions produce after the restart have to
  // name the orders the way the submitters did -- which is the stream half of
  // the digest.
  //
  // The decisions are commands in the stream rather than HoldResponder
  // answers: both holds have to still be open AT the checkpoint, and that is
  // not something a randomized responder can be asked for. `viaClone` routes
  // the checkpoint through the path production takes.
  s.push_back({"snapshot_midstream_held_clordid",
               []
               {
                 SymbolConfig c = spotCfg();
                 c.lastLookWindowNs = DurationNs{1'000'000};
                 c.lastLookAcceptOnTimeout = false;
                 const auto named = [](OrderId id, Side side, double p, double q,
                                       uint64_t acct, uint64_t clOrdId, bool lastLook)
                 {
                   NewOrder o;
                   o.id = id;
                   o.symbol = SYM;
                   o.side = side;
                   o.type = OrderType::LIMIT;
                   o.price = px(p);
                   o.quantity = qty(q);
                   o.accountId = acct;
                   o.clientOrderId = clOrdId;
                   o.lastLook = lastLook;
                   return o;
                 };
                 std::vector<InboundCommand> cmds;
                 // -- the pre-checkpoint half: two holds, four named legs
                 cmds.emplace_back(Deposit{1, QUOTE, {}, quoteRaw(100000.0), SYM});
                 cmds.emplace_back(Deposit{2, BASE, {}, baseRaw(100.0), SYM});
                 cmds.emplace_back(Deposit{3, BASE, {}, baseRaw(100.0), SYM});
                 cmds.emplace_back(Deposit{4, QUOTE, {}, quoteRaw(100000.0), SYM});
                 cmds.emplace_back(named(10, Side::SELL, 100.00, 5.0, 2, 9001, true));
                 cmds.emplace_back(named(11, Side::BUY, 100.00, 5.0, 1, 9002, false));
                 cmds.emplace_back(named(12, Side::SELL, 100.50, 4.0, 3, 9003, true));
                 cmds.emplace_back(named(13, Side::BUY, 100.50, 4.0, 4, 9004, false));
                 // -- the post-restore half: one hold accepted, one refused,
                 //    then the legs the refusal put back are canceled, so the
                 //    ids travel through OrderExecuted, FillRejected,
                 //    OrderModified, OrderAccepted and OrderCanceled alike
                 cmds.emplace_back(InboundCommand{LastLookDecision{1, SYM, true, {}, 2}});
                 cmds.emplace_back(InboundCommand{LastLookDecision{2, SYM, false, {}, 3}});
                 cmds.emplace_back(InboundCommand{CancelOrder{13, SYM, {}, 4}});
                 cmds.emplace_back(InboundCommand{CancelOrder{12, SYM, {}, 3}});
                 cmds.emplace_back(Deposit{1, BASE, {}, baseRaw(10.0), SYM});
                 cmds.emplace_back(named(14, Side::SELL, 101.00, 1.0, 1, 9005, false));
                 cmds.emplace_back(InboundCommand{CancelOrder{14, SYM, {}, 1}});
                 cmds.emplace_back(InboundCommand{CancelOrder{999, SYM, {}, 1}});
                 return checkpointed<Book>(
                     c, cmds, [](Run& r)
                     { r.eng.setLedger(&r.led, VENUE_ACCT); }, false,
                     "golden_snap_held_clordid", /*viaClone*/ true);
               }});

  // QuoteLadder: a maker's whole set of levels in one command. Appended at
  // the end of the corpus on purpose -- a scenario inserted anywhere else
  // would rewrite the ORDER of the rows in the table, and a row that moved
  // reads in a diff exactly like a row whose numbers moved.
  //
  // The ladder is one sequenced command that walks the quote path K times,
  // so what is at stake is the ORDER of what comes out of it: accepts and
  // cancels interleaved level by level, a taker crossing the top rung
  // mid-ladder, a narrower ladder taking down the rungs it stopped naming,
  // and a zero-level ladder pulling the block. None of that is visible in a
  // state digest -- the same orders rest either way -- which is why it is
  // here, where the stream digest is compared too.
  s.push_back({"quote_ladder_replace",
               []
               {
                 SymbolConfig c = spotCfg();
                 Digest d;
                 Run r(c, d);
                 r.eng.setLedger(&r.led, VENUE_ACCT);
                 for (const auto& dep : deposits(3, 1000.0, 1'000'000.0))
                 {
                   r.push(dep);
                 }

                 const auto rung = [](uint8_t levels, double step, double size)
                 {
                   QuoteLadder l;
                   l.accountId = 1;
                   l.symbol = SYM;
                   l.bidIdBase = 100;
                   l.askIdBase = 200;
                   l.levels = levels;
                   l.tif = TimeInForce::GTC;
                   for (uint8_t i = 0; i < levels; ++i)
                   {
                     l.level[i].bidPrice = px(99.99 - step * i);
                     l.level[i].bidQty = qty(size + i);
                     l.level[i].askPrice = px(100.01 + step * i);
                     l.level[i].askQty = qty(size + 1 + i);
                   }
                   return l;
                 };

                 r.push(InboundCommand{rung(5, 0.01, 1.0)});
                 // A taker lifts the top ask while the ladder rests, so the
                 // next ladder replaces a level that is no longer whole.
                 NewOrder take;
                 take.id = 900;
                 take.symbol = SYM;
                 take.side = Side::BUY;
                 take.type = OrderType::LIMIT;
                 take.price = px(100.02);
                 take.quantity = qty(1.5);
                 take.accountId = 2;
                 r.push(InboundCommand{take});

                 r.push(InboundCommand{rung(5, 0.02, 2.0)});                   // reprice in place
                 r.push(InboundCommand{rung(3, 0.02, 2.0)});                   // two rungs dropped
                 r.push(InboundCommand{rung(kQuoteLadderLevels, 0.01, 1.0)});  // full block
                 r.push(InboundCommand{rung(0, 0.01, 1.0)});                   // the whole ladder pulled
                 return r.hashes();
               }});

  return s;
}

// ---- the table ------------------------------------------------------------

const char* tablePath() { return FLOX_VENUE_GOLDEN_TABLE; }

std::map<std::string, Hashes> readTable(bool& found)
{
  std::map<std::string, Hashes> t;
  std::ifstream in(tablePath());
  found = in.good();
  if (!found)
  {
    return t;
  }
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#')
    {
      continue;
    }
    std::istringstream is(line);
    std::string name, a, b, c;
    if (!(is >> name >> a >> b >> c))
    {
      continue;
    }
    Hashes h{};
    h.state = std::strtoull(a.c_str(), nullptr, 16);
    h.config = std::strtoull(b.c_str(), nullptr, 16);
    h.stream = std::strtoull(c.c_str(), nullptr, 16);
    t[name] = h;
  }
  return t;
}

void writeTable(const std::vector<std::pair<std::string, Hashes>>& rows)
{
  std::ofstream out(tablePath(), std::ios::trunc);
  ASSERT_TRUE(out.good()) << "cannot write " << tablePath();
  out << "# Golden replay hashes -- see venue/tests/test_venue_golden_replay.cpp.\n"
      << "#\n"
      << "# One row per scenario: name, then stateHash, configHash and the hash of\n"
      << "# the outbound event stream (plus the snapshot record trace, for the\n"
      << "# checkpoint scenarios). All three are 16 hex digits.\n"
      << "#\n"
      << "# This file is written ONLY by hand, by running the test with\n"
      << "# FLOX_UPDATE_GOLDEN=1, and only when the behaviour change that moved a\n"
      << "# number is intended and explained in the same commit. A diff here is a\n"
      << "# claim that the venue now behaves differently; it is never housekeeping.\n"
      << "#\n"
      << "# scenario                               state             config            stream\n";
  for (const auto& [name, h] : rows)
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%-38s %s  %s  %s\n", name.c_str(), hex(h.state).c_str(),
                  hex(h.config).c_str(), hex(h.stream).c_str());
    out << buf;
  }
}

bool updateRequested()
{
  const char* e = std::getenv("FLOX_UPDATE_GOLDEN");
  return e != nullptr && e[0] != '\0' && e[0] != '0';
}

}  // namespace

TEST(VenueGoldenReplay, CorpusMatchesTheTable)
{
  const auto scenarios = corpus<MatchingBook>();
  std::vector<std::pair<std::string, Hashes>> produced;
  produced.reserve(scenarios.size());
  for (const Scenario& s : scenarios)
  {
    produced.emplace_back(s.name, s.run());
  }

  // A scenario that emits nothing pins nothing: it would keep passing through
  // any change at all. Cheap to check, and the corpus is exactly the kind of
  // thing that rots quietly when a generator stops reaching the engine.
  Coverage total{};
  for (const auto& [name, got] : produced)
  {
    uint64_t events = 0;
    for (size_t k = 0; k < kProbes; ++k)
    {
      events += got.n[k];
      total[k] += got.n[k];
    }
    EXPECT_GT(events, 0u) << "scenario '" << name << "' emitted no events at all";
  }

  std::printf("[ CORPUS   ] %zu scenarios\n", produced.size());
  for (size_t k = 0; k < kProbes; ++k)
  {
    std::printf("[ COVERAGE ] %-22s %llu\n", kProbeNames[k],
                static_cast<unsigned long long>(total[k]));
    // An event kind no scenario produces is an event kind this test does not
    // watch: the refactor could stop emitting it and every hash would agree.
    EXPECT_GT(total[k], 0u) << "no scenario in the corpus emits " << kProbeNames[k]
                            << "; the golden replay cannot see it change";
  }

  if (updateRequested())
  {
    writeTable(produced);
    std::printf("FLOX_UPDATE_GOLDEN: rewrote %s with %zu scenarios.\n", tablePath(),
                produced.size());
    std::printf("Review the diff: every moved number is a behaviour change.\n");
    return;
  }

  bool found = false;
  const auto table = readTable(found);
  ASSERT_TRUE(found) << "golden table missing: " << tablePath()
                     << "\nRun with FLOX_UPDATE_GOLDEN=1 to create it.";

  for (const auto& [name, got] : produced)
  {
    const auto it = table.find(name);
    if (it == table.end())
    {
      ADD_FAILURE() << "scenario '" << name << "' has no row in " << tablePath()
                    << "\n  produced state=" << hex(got.state) << " config=" << hex(got.config)
                    << " stream=" << hex(got.stream)
                    << "\n  A new scenario needs its row added with FLOX_UPDATE_GOLDEN=1.";
      continue;
    }
    const Hashes& want = it->second;
    if (got.state != want.state || got.config != want.config || got.stream != want.stream)
    {
      std::string what;
      if (got.state != want.state)
      {
        what += "\n  stateHash   expected " + hex(want.state) + "  got " + hex(got.state);
      }
      if (got.config != want.config)
      {
        what += "\n  configHash  expected " + hex(want.config) + "  got " + hex(got.config);
      }
      if (got.stream != want.stream)
      {
        what += "\n  streamHash  expected " + hex(want.stream) + "  got " + hex(got.stream);
      }
      ADD_FAILURE() << "GOLDEN REPLAY DIVERGED in scenario '" << name << "'" << what
                    << "\n  The engine behaves differently than the recorded run."
                    << "\n  If that is intended, rerun with FLOX_UPDATE_GOLDEN=1 and put the"
                    << "\n  rewritten " << tablePath() << " in the same commit as the change.";
    }
  }

  for (const auto& [name, want] : table)
  {
    (void)want;
    const bool present = std::any_of(produced.begin(), produced.end(),
                                     [&](const auto& p)
                                     { return p.first == name; });
    if (!present)
    {
      ADD_FAILURE() << "scenario '" << name << "' is in " << tablePath()
                    << " but no longer in the corpus. A scenario that stopped running"
                    << " protects nothing; remove the row deliberately.";
    }
  }

  // LadderBook: the SAME corpus, checked against the SAME table -- no second
  // replay_hashes_ladder.txt. The contract comment above corpus<Book>()
  // works out why the table is exactly as strong a check for LadderBook as
  // for MatchingBook: stateHash/configHash/stream hash fold in only cfg_ and
  // RestingOrder content via Book::forEachOrder's canonical traversal, never
  // a book's own representation, so the two engines are contractually
  // required to land on the SAME three numbers per scenario. This is what
  // catches a mutation at LadderBook's two real-fill points
  // (LadderBook::fillBest, LadderBook::consumeById) that the MatchingBook
  // pass above cannot see by construction -- the gap T058 found and left
  // open (see .notes/tracks/W26-venue-hardening/T058-fix-order-canceled-leaves-qty.md,
  // "Mutation-coverage follow-up").
  for (const Scenario& s : corpus<LadderBook>())
  {
    const auto it = table.find(s.name);
    if (it == table.end())
    {
      continue;  // already reported above, by the MatchingBook pass
    }
    const Hashes got = s.run();
    const Hashes& want = it->second;
    if (got.state != want.state || got.config != want.config || got.stream != want.stream)
    {
      std::string what;
      if (got.state != want.state)
      {
        what += "\n  stateHash   expected " + hex(want.state) + "  got " + hex(got.state);
      }
      if (got.config != want.config)
      {
        what += "\n  configHash  expected " + hex(want.config) + "  got " + hex(got.config);
      }
      if (got.stream != want.stream)
      {
        what += "\n  streamHash  expected " + hex(want.stream) + "  got " + hex(got.stream);
      }
      ADD_FAILURE() << "LADDERBOOK GOLDEN REPLAY DIVERGED in scenario '" << s.name << "'" << what
                    << "\n  MatchingEngine<LadderBook> disagrees with the SAME table row"
                    << " MatchingEngine<MatchingBook> matches."
                    << "\n  By contract (see the comment above corpus<Book>() in this file) the"
                    << " two must agree exactly; this is never fixed by FLOX_UPDATE_GOLDEN,"
                    << "\n  which only ever records the MatchingBook pass.";
    }
  }
}

// The finding the golden replay produced on its first run against the other
// standard library, kept as a check of its own.
//
// An emergency cancel sweeps the resting book and then the pending
// conditionals. The resting half had always been sorted before it was
// published; the conditional half was published in std::unordered_map
// traversal order, which is a bucket-layout artifact and differs between
// libstdc++ and libc++ for the same insertions. The resulting STATE is
// identical either way -- every order is gone -- so no state-hash comparison
// in this suite could see it; only the event stream differs, and only on a
// venue built against the other library.
//
// The golden table would catch a regression here again, but only on whichever
// library the table was not recorded on. This does not care: it asserts the
// property directly.
TEST(VenueGoldenReplay, EmergencyCancelReportsPendingStopsInIdOrder)
{
  Digest d;
  EngineRun r(spotCfg(), d);

  // Ids out of order on purpose, and spread out, so a map traversal has every
  // chance to hand them back in some other sequence.
  const OrderId ids[] = {907, 13, 55, 2, 471, 88, 306, 7, 1201, 64, 39, 750};
  for (OrderId id : ids)
  {
    NewOrder o;
    o.id = id;
    o.symbol = SYM;
    o.side = Side::SELL;
    o.type = OrderType::STOP_MARKET;
    o.quantity = qty(1.0);
    o.triggerPrice = px(90.0);  // no trade and no mark yet: nothing triggers
    o.accountId = 1 + (id % 5);
    r.push(InboundCommand{o});
  }
  ASSERT_EQ(r.eng.book().empty(), true) << "a conditional order reached the resting book";

  r.push(InboundCommand{AdminCmd{SYM, AdminAction::HaltAndCancelAll}});

  std::vector<OrderId> canceled;
  for (const OutboundEvent& e : r.since)
  {
    if (const auto* c = std::get_if<OrderCanceled>(&e))
    {
      canceled.push_back(c->id);
    }
  }
  ASSERT_EQ(canceled.size(), std::size(ids)) << "the sweep did not report every pending order";
  EXPECT_TRUE(std::is_sorted(canceled.begin(), canceled.end()))
      << "pending conditionals were canceled in map-traversal order: the event stream of an "
         "emergency cancel now depends on the standard library the venue was built with";
}
