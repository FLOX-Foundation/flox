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
#include "support/engine_surface.h"
#include "support/recovery_scenario.h"
#include "support/tmp_path.h"

#include "flox/backtest/fee_schedule.h"

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

// One engine plus the digest its events fold into. Not copyable or movable:
// the sink captures `this`, and a moved Run would keep feeding the old one.
struct Run
{
  Digest& dig;
  Digest* tap{nullptr};  // optional second digest, for the continuity check
  Ledger led;
  std::vector<OutboundEvent> since;  // events of the last submit
  MatchingEngine<MatchingBook> eng;
  int64_t ts{1};

  Run(const SymbolConfig& c, Digest& d)
      : dig(d), eng(c, [this](const OutboundEvent& e)
                    {
                       dig.event(e);
                       if (tap != nullptr)
                       {
                         tap->event(e);
                       }
                       since.push_back(e); })
  {
  }
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;

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

using Setup = std::function<void(Run&)>;

// gtest's fixture already has a member named Run, so a TEST body cannot name
// the type unqualified.
using EngineRun = Run;

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

  void observe(const Run& r, int64_t nowTs)
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

  void drain(Run& r, SymbolId sym)
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
      r.push(InboundCommand{LastLookDecision{p.heldId, sym, p.accept, p.account}});
    }
  }
};

void drive(Run& r, const std::vector<InboundCommand>& cmds, HoldResponder* holds, SymbolId sym)
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
      v.emplace_back(Deposit{a, BASE, baseRaw(baseAmt), SYM});
    }
    v.emplace_back(Deposit{a, QUOTE, quoteRaw(quoteAmt), SYM});
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

Hashes plain(const SymbolConfig& c, const std::vector<InboundCommand>& cmds, const Setup& setup,
             bool lastLook, int64_t finalTick)
{
  Digest d;
  Run r(c, d);
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
Hashes checkpointed(const SymbolConfig& c, const std::vector<InboundCommand>& cmds,
                    const Setup& setup, bool lastLook, const char* pathStem,
                    bool viaClone = false)
{
  const size_t half = cmds.size() / 2;

  Digest contD;
  Digest contTail;
  Run cont(c, contD);
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
    Run a(c, d);
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
        auto clone = a.eng.cloneForSnapshot();
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

    Run b(c, d);
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

std::vector<Scenario> corpus()
{
  std::vector<Scenario> s;

  // The one command stream the journal/recovery tests are built on, driven
  // through the engine directly. It is the closest thing this tree has to a
  // journal fixture: there is no .bin in the repo because the recovery tests
  // generate their journals from this generator at run time.
  s.push_back({"journal_recovery_scenario",
               []
               {
                 return plain(test::scenarioConfig(), test::scenarioCommands(), [](Run& r)
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
                 return plain(spotCfg(), workload::mixedFlow(params(0xC0FFEE123456789ULL, 20000)),
                              nullptr, false, 3'000'000);
               }});

  // The same flow with a ledger and a fee schedule bound: settlement,
  // reservations, BalanceUpdate and FeeCharged join the stream.
  s.push_back({"mixed_flow_cleared",
               []
               {
                 return plain(
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
                 return plain(c, concat(deposits(6, 100000.0, 10'000'000.0), workload::makerFlow(params(0xA11CE5EEDULL, 8000, 6))), [](Run& r)
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
                 return plain(c, concat(deposits(6, 100000.0, 10'000'000.0), workload::makerFlow(params(0xBEEF1234ULL, 8000, 6))), [](Run& r)
                              { r.eng.setLedger(&r.led, VENUE_ACCT); }, true, 0);
               }});

  // MM protection and the mass-cancel / quote-replace path.
  s.push_back({"mmp_quote_mass_cancel",
               []
               {
                 return plain(spotCfg(), concat(deposits(6, 100000.0, 10'000'000.0), workload::makerFlow(params(0x5EED0001ULL, 8000, 6))), [](Run& r)
                              {
                                r.eng.setLedger(&r.led, VENUE_ACCT);
                                for (uint64_t a = 1; a <= 6; ++a)
                                {
                                  r.eng.setMmp(a, qty(12.0), DurationNs{500});
                                } }, false, 0);
               }});

  // Self-trade prevention across a firm -- two accounts in one group are one
  // trader, and the flow carries all four modes -- plus admission profiles,
  // which decide what a given account is allowed to send at all.
  s.push_back({"stp_groups_and_admission",
               []
               {
                 return plain(
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
                 return plain(perpCfg(/*adl*/ false), concat(deposits(8, 0.0, 200000.0), workload::perpFlow(params(0x9DEADBEEFULL, 15000))), [](Run& r)
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
                 return plain(perpCfg(/*adl*/ true), concat(concat(deposits(8, 0.0, 600.0), {InboundCommand{Deposit{VENUE_ACCT, QUOTE, quoteRaw(500000.0), SYM}}}), workload::perpFlow(p)), [](Run& r)
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
                 r.push(InboundCommand{Deposit{1, QUOTE, quoteRaw(250.0), SYM}});
                 r.push(InboundCommand{Deposit{2, QUOTE, quoteRaw(50000.0), SYM}});
                 r.push(InboundCommand{Deposit{VENUE_ACCT, QUOTE, quoteRaw(100.0), SYM}});

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

                 r.push(InboundCommand{SetMark{SYM, px(100.0)}});
                 r.push(InboundCommand{SetMark{SYM, px(60.0)}});
                 return r.hashes();
               }});

  // Checkpoint in the middle of a cleared spot stream.
  s.push_back({"snapshot_midstream_spot",
               []
               {
                 return checkpointed(
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
                     false, "golden_snap_spot");
               }});

  // Checkpoint with open holds, MMP windows and STP groups in flight: the
  // parts of the snapshot that are engine state rather than book state.
  s.push_back({"snapshot_midstream_lastlook",
               []
               {
                 SymbolConfig c = spotCfg();
                 c.lastLookWindowNs = DurationNs{40};
                 c.clOrdIdWindowNs = 2000;
                 return checkpointed(
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
                     true, "golden_snap_lastlook");
               }});

  // Checkpoint with open perp positions, posted margin and a funding
  // calendar: RestorePosition, RestoreReservation and RestoreFunding.
  s.push_back({"snapshot_midstream_perp",
               []
               {
                 return checkpointed(
                     perpCfg(/*adl*/ false),
                     concat(deposits(8, 0.0, 200000.0),
                            workload::perpFlow(params(0x9E97AA51ULL, 8000))),
                     [](Run& r)
                     {
                       r.eng.setLedger(&r.led, VENUE_ACCT);
                       r.eng.setFundingSchedule(DurationNs{8000}, SeqNanos::fromRaw(8000));
                     },
                     false, "golden_snap_perp");
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
                 cmds.emplace_back(Deposit{1, BASE, baseRaw(100.0), SYM});
                 cmds.emplace_back(Deposit{1, QUOTE, quoteRaw(100000.0), SYM});
                 cmds.emplace_back(Deposit{2, QUOTE, quoteRaw(100000.0), SYM});
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
                 cmds.emplace_back(InboundCommand{CancelOrder{999, SYM, 0}});
                 cmds.emplace_back(InboundCommand{Deposit{2, BASE, baseRaw(1.0), SYM}});
                 return checkpointed(
                     c, cmds, [](Run& r)
                     { r.eng.setLedger(&r.led, VENUE_ACCT); }, false,
                     "golden_snap_delisted", /*viaClone*/ true);
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
  const auto scenarios = corpus();
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
