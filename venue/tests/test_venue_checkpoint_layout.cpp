/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The ORDER of the records a snapshot is made of.
 *
 * The state hash says the engine's state is what it was; the golden replay
 * says the behaviour is. Neither of them sees the snapshot's LAYOUT. Swap two
 * sections of writeSnapshot and both stay green as long as every record is
 * still written, because restore is driven by each record's own tag rather
 * than by its position in the file.
 *
 * The layout is a format promise all the same. Another build reads these
 * files; RestoreBalance before RestoreReservation is the difference between
 * an exact restore and a re-reservation out of a total; a config record that
 * moved after the Restore* records would replay through the live submit path
 * onto state that is already rebuilt. So the sequence is written down here,
 * where moving a section has to be typed out deliberately.
 *
 * The expected sequence is RECORDED, not derived: taken from a run of this
 * test on the commit named next to the constant, before the checkpoint
 * functions were composed out of their components.
 */
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
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

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
int64_t baseRaw(double v) { return static_cast<int64_t>(amountOf(qty(v))); }
int64_t quoteRaw(double v) { return static_cast<int64_t>(amountOf(Volume::fromDouble(v))); }

// Every alternative of InboundCommand, by its position in the variant: what a
// record is reported and recorded under. Names rather than tag numbers -- a
// section that moved should read as "RestorePosition went before
// RestoreHeld", not as "20 and 19 swapped".
constexpr const char* kCommandName[] = {"NewOrder",
                                        "CancelOrder",
                                        "ModifyOrder",
                                        "MassCancel",
                                        "Quote",
                                        "LastLookDecision",
                                        "SetMark",
                                        "ApplyFunding",
                                        "AdminCmd",
                                        "Deposit",
                                        "Withdraw",
                                        "ListInstrument",
                                        "SetBands",
                                        "TimeTick",
                                        "SetTriggerRef",
                                        "SnapshotBegin",
                                        "RestoreOrder",
                                        "RestoreStop",
                                        "RestorePeg",
                                        "RestoreHeld",
                                        "RestorePosition",
                                        "RestoreMmpCfg",
                                        "RestoreClOrdIds",
                                        "SnapshotEnd",
                                        "RestoreReservation",
                                        "RestoreBalance",
                                        "RestoreMmpFills",
                                        "SetStpGroup",
                                        "SetFundingSchedule",
                                        "RestoreFunding",
                                        "ForceClosePosition",
                                        "RestoreOrderStp",
                                        "SetAdmissionProfile",
                                        "SetRiskLimits",
                                        "AdjustPosition",
                                        "QuoteLadder",
                                        "SetAccountRiskLimits"};

static_assert(std::size(kCommandName) == std::variant_size_v<InboundCommand>,
              "a new InboundCommand alternative needs its name here, or a record of it would be "
              "reported as an unnamed number");

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  // Last look open: a hold is the one piece of engine state that exists only
  // while a fill is unprinted, and it has a snapshot record of its own.
  c.lastLookWindowNs = DurationNs{100'000'000};
  c.lastLookAcceptOnTimeout = false;
  // Linear perp: positions and posted margin, so clearing has something to
  // write.
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.maintenanceMarginBps = 500;
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

// One engine with every checkpoint section non-empty: balances, positions,
// funding, reservations, an open hold, pegs, per-order STP, firm STP groups,
// an admission profile, GTD deadlines, OCO across both books, MMP config and
// MMP window fills, clientOrderId dedup, and a session that is both halted
// and closed. A section that fell out of this scenario would leave its part
// of the layout unpinned, which is what the second test checks.
void buildEverySection(MatchingEngine<MatchingBook>& eng)
{
  int64_t ts = 1'000'000;
  const auto send = [&](const InboundCommand& c)
  {
    ts += 1000;
    eng.submit(c, ts);
  };

  eng.setMmp(1, qty(1000.0), DurationNs{1'000'000'000});
  eng.setMmp(2, qty(1000.0), DurationNs{1'000'000'000});
  eng.setStpGroup(3, 77);
  eng.setStpGroup(4, 77);
  AdmissionProfile prof;
  // Every type and every TIF spelled out rather than the 0 that means "all":
  // a mask that is actually enumerated is the interesting case, and a mask
  // that is short of a bit silently rejects the order that needs it (the
  // trailing stop below is OrderType bit 6).
  prof.allowedTypes = 0xFF;
  prof.allowedTif = 0x1F;
  eng.setAdmissionProfile(5, prof);
  SetAccountRiskLimits caps;
  caps.symbol = SYM;
  caps.fields = AccountRiskLimitField::AccountRiskFatFinger | AccountRiskLimitField::AccountRiskMaxPosition;
  caps.account = 5;
  caps.maxOrderQty = qty(500.0);
  caps.maxPositionQty = qty(2000.0);
  eng.setAccountRiskLimits(caps);
  eng.setFundingSchedule(DurationNs{28'800'000'000'000}, SeqNanos::fromRaw(50'000'000));

  for (uint64_t acct = 1; acct <= 5; ++acct)
  {
    send(InboundCommand{Deposit{acct, QUOTE, {}, quoteRaw(100000), SYM}});
  }
  send(InboundCommand{Deposit{1, BASE, {}, baseRaw(1000), SYM}});
  send(InboundCommand{Deposit{2, BASE, {}, baseRaw(1000), SYM}});

  // A printed perp trade: positions and posted margin for accounts 1 and 2,
  // and one MMP window fill for the maker.
  send(InboundCommand{limit(1, Side::SELL, 100.00, 5.0, 2)});
  send(InboundCommand{limit(2, Side::BUY, 100.00, 5.0, 1)});
  send(InboundCommand{SetMark{SYM, {}, px(100.0)}});

  // A last-look maker and the taker that takes part of it: the remainder
  // rests, the taken part stays an open hold.
  //
  // Both legs carry a clientOrderId, so the RestoreHeld record in the pinned
  // file is the fully-populated one: the two ids are the part of it the state
  // hash folds in, and the third test below is what says the file this
  // layout describes still restores.
  NewOrder maker = limit(10, Side::SELL, 101.00, 5.0, 3);
  maker.lastLook = true;
  maker.clientOrderId = 554;
  maker.visibleQuantity = qty(2.0);  // iceberg: hidden reserve rides RestoreOrder
  send(InboundCommand{maker});
  NewOrder taker = limit(11, Side::BUY, 101.00, 2.0, 4);
  taker.tif = TimeInForce::IOC;
  taker.clientOrderId = 555;
  send(InboundCommand{taker});

  NewOrder gtd = limit(12, Side::BUY, 99.00, 2.0, 4);
  gtd.tif = TimeInForce::GTD;
  gtd.expiryNs = SeqNanos::fromRaw(500'000'000);
  send(InboundCommand{gtd});

  NewOrder ocoBook = limit(13, Side::BUY, 98.00, 2.0, 4);
  ocoBook.ocoGroup = 7;
  send(InboundCommand{ocoBook});
  NewOrder ocoStop = limit(14, Side::SELL, 0.0, 2.0, 5);
  ocoStop.type = OrderType::STOP_MARKET;
  ocoStop.triggerPrice = px(90.0);
  ocoStop.ocoGroup = 7;
  send(InboundCommand{ocoStop});

  NewOrder trail = limit(15, Side::SELL, 0.0, 1.0, 5);
  trail.type = OrderType::TRAILING_STOP;
  trail.trailingOffset = px(1.00);
  send(InboundCommand{trail});

  NewOrder peg = limit(16, Side::BUY, 0.0, 1.0, 5);
  peg.peg = PegRef::Bid;
  send(InboundCommand{peg});

  NewOrder stp = limit(17, Side::BUY, 97.00, 1.0, 3);
  stp.stp = STPMode::CancelNewest;
  send(InboundCommand{stp});

  NewOrder clord = limit(18, Side::BUY, 96.00, 1.0, 4);
  clord.clientOrderId = 556;
  send(InboundCommand{clord});

  // Last: neither transition pulls the book (see the session automaton), so
  // the state above is intact and the two admin records ride the file.
  eng.setHalted(true);
  eng.closeSession();
}

// The records of a snapshot, in file order, by name.
std::vector<std::string> snapshotLayout(const MatchingEngine<MatchingBook>& eng,
                                        const std::string& path)
{
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    eng.writeSnapshot(out);
    out.flush();
  }
  std::vector<std::string> names;
  for (const auto& [ts, cmd] : Journal::loadTimed(path))
  {
    (void)ts;
    names.emplace_back(kCommandName[cmd.index()]);
  }
  return names;
}

// The recorded layout. Do not regenerate it to make a diff go away: each
// entry is a record position in a file another build reads.
//
// recorded on 1fb24b4aa0604a5974fc94b5450f30219ff32fb7 (origin/main) by this
// test, before the checkpoint functions were composed out of their
// components. One entry has been added since: naming the hold's maker gave
// account 3 a dedup entry of its own, so the clOrdId section writes two
// records where it wrote one. No section moved, and the hold is still the
// single RestoreHeld it always was.
// clang-format off
const std::vector<std::string> kRecordedLayout = {
    "SnapshotBegin",
    // the config section: replayed through the live submit path on load, so
    // it has to precede everything it decides the admission of
    "ListInstrument", "SetBands", "SetRiskLimits", "SetTriggerRef",
    "SetStpGroup", "SetStpGroup",            // matcher: firm groups, by account
    "SetAdmissionProfile",                   // credit: who may send what
    "SetAccountRiskLimits",                  // credit: one account's own caps (W26-T064), after the
                                             // profile that admits it and before anything it bounds
    "AdminCmd", "AdminCmd",                  // session: Halt, then CloseSession
    "RestoreFunding",                        // clearing: the calendar and the rate
    // the ledger: exact signed available/reserved per (account, asset). Ahead
    // of the reservations and the positions, which then rebuild only the
    // engine-side tables instead of re-reserving.
    "RestoreBalance", "RestoreBalance", "RestoreBalance", "RestoreBalance",
    "RestoreBalance", "RestoreBalance", "RestoreBalance",
    "RestoreMmpCfg", "RestoreMmpCfg",        // market-maker protection: config
    "RestoreMmpFills", "RestoreMmpFills",    // ... then the window fills
    // clientOrderId dedup generations, one record per account with ids: the
    // hold's maker (account 3) and the two account-4 orders
    "RestoreClOrdIds", "RestoreClOrdIds",
    // the book: levels best-first, FIFO within, so tail-appending restore
    // reproduces the live layout
    "RestoreOrder", "RestoreOrder", "RestoreOrder", "RestoreOrder",
    "RestoreOrder", "RestoreOrder",
    "RestoreStop", "RestoreStop",            // the stop book, by order id
    "RestorePeg",                            // pegs: must reference a restored order
    "RestoreOrderStp",                       // per-order STP modes
    "RestorePosition", "RestorePosition",    // clearing: positions and margin
    "RestoreHeld",                           // last look: the unprinted fill
    "RestoreReservation", "RestoreReservation", "RestoreReservation",
    "RestoreReservation", "RestoreReservation", "RestoreReservation",
    "RestoreReservation",
    "SnapshotEnd"};
// clang-format on

}  // namespace

// The sequence itself. A swap of two sections in writeSnapshot lands here and
// nowhere else.
TEST(VenueCheckpointLayout, RecordSequenceIsPinned)
{
  const std::string path = tmpPath("checkpoint_layout", ".snap");
  std::remove(path.c_str());

  Ledger led;
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  buildEverySection(eng);

  const std::vector<std::string> actual = snapshotLayout(eng, path);
  EXPECT_EQ(actual, kRecordedLayout);
  std::remove(path.c_str());
}

// The reference is only worth what the scenario covers: every section the
// checkpoint can write has to be represented, or a record type could move
// unnoticed because this engine never had any.
TEST(VenueCheckpointLayout, EverySectionIsExercised)
{
  const std::string path = tmpPath("checkpoint_layout_cover", ".snap");
  std::remove(path.c_str());

  Ledger led;
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  buildEverySection(eng);

  const std::vector<std::string> actual = snapshotLayout(eng, path);
  for (const char* section : {"SnapshotBegin", "ListInstrument", "SetBands", "SetRiskLimits",
                              "SetTriggerRef", "SetStpGroup", "SetAdmissionProfile",
                              "SetAccountRiskLimits", "AdminCmd",
                              "RestoreFunding", "RestoreBalance", "RestoreMmpCfg",
                              "RestoreMmpFills", "RestoreClOrdIds", "RestoreOrder", "RestoreStop",
                              "RestorePeg", "RestoreOrderStp", "RestorePosition", "RestoreHeld",
                              "RestoreReservation", "SnapshotEnd"})
  {
    EXPECT_NE(std::find(actual.begin(), actual.end(), section), actual.end())
        << "no " << section << " record: that section of the layout is unpinned";
  }
  std::remove(path.c_str());
}

// The scenario's snapshot must also be a valid one: the file restores into a
// fresh engine and the state hashes agree. Without this the layout could be
// pinned on a snapshot nothing can read.
TEST(VenueCheckpointLayout, TheSnapshotItPinsStillRestores)
{
  const std::string path = tmpPath("checkpoint_layout_restore", ".snap");
  std::remove(path.c_str());

  Ledger led;
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  buildEverySection(eng);
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    eng.writeSnapshot(out);
    out.flush();
  }

  Ledger led2;
  MatchingEngine<MatchingBook> rec(cfg(), [](const OutboundEvent&) {});
  rec.setLedger(&led2, VENUE_ACCT);
  const auto records = Journal::loadTimed(path);
  ASSERT_GE(records.size(), 2u);
  for (const auto& [ts, cmd] : records)
  {
    ASSERT_TRUE(rec.applySnapshotRecord(cmd, ts)) << "record " << kCommandName[cmd.index()];
  }
  EXPECT_EQ(rec.stateHash(), eng.stateHash());
  std::remove(path.c_str());
}

// cloneForSnapshot is the fourth function of the checkpoint, and the one the
// asynchronous path actually serializes: the background writer never sees the
// live engine, only the clone. So the clone has to write the same file -- a
// component the clone forgot to copy is a section of that file that quietly
// goes missing, and neither the golden replay nor the layout reference above
// would notice, because both look only at the live engine.
//
// Compared as the record sequence, the byte COUNT and the state hash rather
// than as raw bytes: a journaled body is a blitted struct and some of them
// carry padding (SnapshotBegin has four bytes of it between formatVersion
// and lastAppliedTs), which is written uninitialised, so two snapshots of one
// state are equal field for field and not byte for byte.
TEST(VenueCheckpointLayout, TheCloneWritesTheSameFile)
{
  const std::string live = tmpPath("checkpoint_layout_live", ".snap");
  const std::string copy = tmpPath("checkpoint_layout_clone", ".snap");
  std::remove(live.c_str());
  std::remove(copy.c_str());

  Ledger led;
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  buildEverySection(eng);

  auto clone = eng.cloneForSnapshot();
  ASSERT_NE(clone.engine, nullptr);
  EXPECT_EQ(clone.engine->stateHash(), eng.stateHash());

  uint64_t liveBytes = 0;
  uint64_t cloneBytes = 0;
  {
    Journal out(live, Journal::Sync::Off, Journal::OpenMode::Truncate);
    eng.writeSnapshot(out);
    out.flush();
    liveBytes = out.bytes();
  }
  {
    Journal out(copy, Journal::Sync::Off, Journal::OpenMode::Truncate);
    clone.engine->writeSnapshot(out);
    out.flush();
    cloneBytes = out.bytes();
  }
  EXPECT_EQ(cloneBytes, liveBytes);

  std::vector<std::string> liveNames;
  for (const auto& [ts, cmd] : Journal::loadTimed(live))
  {
    (void)ts;
    liveNames.emplace_back(kCommandName[cmd.index()]);
  }
  std::vector<std::string> cloneNames;
  for (const auto& [ts, cmd] : Journal::loadTimed(copy))
  {
    (void)ts;
    cloneNames.emplace_back(kCommandName[cmd.index()]);
  }
  EXPECT_EQ(cloneNames, liveNames);
  EXPECT_EQ(liveNames, kRecordedLayout);
  std::remove(live.c_str());
  std::remove(copy.c_str());
}
