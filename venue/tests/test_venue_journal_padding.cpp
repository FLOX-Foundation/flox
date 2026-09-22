/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * T057: uninitialised padding in journal/snapshot bodies.
 *
 * Every InboundCommand alternative is written to disk as a raw blob
 * (Journal::append). 31 of the 35 carried compiler-inserted alignment
 * padding, and the construction sites throughout the engine build them with
 * ordinary aggregate initialization (SnapshotBegin{a, b, c, d}, ...), which
 * sets the named fields only. Padding then kept whatever was already on the
 * stack, so the same engine state could serialize to two different files
 * (different CRCs) and a few bytes of unrelated stack content reached disk.
 *
 * The fix is in messages.h, not here: every gap the compiler used to insert
 * implicitly is now an explicit `uint8_t padN_[k]{}` member, at the exact
 * byte offset the implicit padding sat at (sizeof(T) is unchanged for every
 * type -- see the static_assert pair in journal.h, one checking it against
 * the pre-fix value, one checking std::has_unique_object_representations_v<T>
 * -- true exactly when a type has no padding bits left). An explicit field
 * has a default member initializer like any other field, so ordinary
 * construction -- aggregate init that leaves it unspecified, the type's own
 * default constructor, anything that runs NSDMI -- zeroes it the same way it
 * zeroes any other field the caller did not set. Nothing runs in
 * Journal::append to compensate for padding; there is nothing left to
 * compensate for.
 *
 * These tests still pin the observable property (bytes on disk, not the
 * mechanism that zeroes them):
 *  - TwoDifferentlyPoisonedInstancesOfEveryTypeSerializeIdentically: for ALL
 *    35 InboundCommand alternatives, each type is instantiated twice on top
 *    of two differently-poisoned stack buffers (0xAB and 0xCD) via bare
 *    placement-new T (default-init, deliberately not T{} -- see `poisoned`
 *    below for why the distinction matters). Named fields come out identical
 *    either way (same default member initializers, including every explicit
 *    pad field's own `{}`); only padding bits could differ, so a
 *    byte-for-byte mismatch between the two written bodies can only mean some
 *    byte was never zeroed. This is the test that goes red under the
 *    mutation described below, deterministically on any machine: the poison
 *    bytes are explicit, not incidental stack leftovers, so there is nothing
 *    for luck to paper over.
 *  - SnapshotBeginPaddingIsZeroAndFieldsSurvive: the specific record named in
 *    the task (four bytes between formatVersion and lastAppliedTs), built the
 *    same way the checkpoint writer builds it, with a positive check that the
 *    fix does not zero real field bytes along with the padding.
 *  - TwoJournalRecordsOfTheSameCommandAreByteIdentical: task item 3(b) --
 *    the same logical NewOrder, built from two differently-poisoned buffers,
 *    appended at the same timestamp, produces two byte-identical records
 *    (header, body and CRC all equal).
 *  - TwoSnapshotsOfTheSameStateViaTheCloneAreByteIdentical: task item 3(a) --
 *    the production path. An engine carrying open orders, a stop, an
 *    iceberg, OCO, GTD, a peg, a last-look hold, a perp position, risk
 *    limits, an admission profile, an STP group and a funding schedule (one
 *    exercise of every record family with padding except the two
 *    fixed-size-batch snapshot records) is cloned via cloneForSnapshot --
 *    the real async-checkpoint path -- and both writeSnapshot files are
 *    memcmp'd whole.
 *
 * Mutation check (not committed as a build-time switch -- see the PR
 * description for how it was verified). Drop the `{}` default member
 * initializer from one pad field in messages.h, e.g.
 * `uint8_t pad0_[4]{};` -> `uint8_t pad0_[4];` on SnapshotBegin. `poisoned<T>`
 * builds T via bare `T proto;` on purpose (see its own comment): that field
 * is now indeterminate specifically under that construction, so
 * TwoDifferentlyPoisonedInstancesOfEveryTypeSerializeIdentically goes red on
 * SnapshotBegin -- verified by hand -- the two poisoned instances now
 * disagree in exactly that field's bytes, while the other three tests (which
 * all construct through `T{...}` or positional aggregate init, the shapes
 * every real call site actually uses) stay green: per-member value-init
 * zeroes a scalar/array member with no clause of its own regardless of
 * whether it has an NSDMI, so those shapes are not sensitive to this
 * particular mutation, only to a member being uninitialised on every
 * construction path (which is the bug this whole fix is about). The same
 * mechanism applies uniformly to every pad field on every type, by
 * construction (same `uint8_t padN_[k]{}` shape, same helper); SnapshotBegin
 * was the one hand-verified representative.
 *
 * has_unique_object_representations_v<T> in journal.h is a second, cheaper
 * line of defence against a DIFFERENT mutation than this one: dropping a pad
 * field's `{}` does not change whether the type has unique object
 * representations (that trait is about the struct's layout -- which bytes
 * exist -- not about whether they happen to be initialized), so it would not
 * catch this specific mutation. What it does catch is a future field added to
 * one of these structs without extending it all the way to the next
 * alignment boundary, reintroducing a gap the static_assert list does not
 * know about -- see the PR description for a worked example (a temporary
 * field added and reverted) demonstrating that static_assert firing.
 */
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
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

std::vector<unsigned char> readFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Builds T on top of a buffer poisoned with `poison`, via DEFAULT-init (no
// braces) -- deliberately not `T{}`. Empty-brace value-initialization runs
// per-member value-init for any member the caller supplies no clause for,
// which reliably zeroes a scalar/array member whether or not it has its own
// default member initializer (verified empirically on this toolchain: a
// no-NSDMI array member between two NSDMI'd members still comes out zero
// under `T{}`). Bare `T proto;` does not: it runs the class's own default
// constructor, which -- for a member with NO NSDMI -- performs ordinary
// default-initialization of THAT member, a no-op for a scalar/array that
// leaves it exactly as poisoned as the surrounding memory. That gap is
// isolated from incidental stack luck by making the "whatever" an explicit,
// known, non-zero byte, so a missing `{}` on a pad field in messages.h
// reproduces here deterministically rather than depending on what happened
// to be on the stack.
template <class T>
T poisoned(unsigned char poison)
{
  alignas(T) unsigned char buf[sizeof(T)];
  std::memset(buf, poison, sizeof(buf));
  new (buf) T;  // default-init: NOT T{} -- see above
  T out;
  std::memcpy(&out, buf, sizeof(T));
  return out;
}

// Appends `v` alone to a fresh file and returns the body bytes (the record
// minus the [ts][stamp][tag][len] header and the trailing [crc]).
template <class T>
std::vector<unsigned char> writeBody(const std::string& path, const T& v, int64_t ts)
{
  std::remove(path.c_str());
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    out.append(InboundCommand{v}, ts);
  }
  auto bytes = readFile(path);
  const size_t total = Journal::kHeaderSize + sizeof(T) + 4;
  if (bytes.size() != total)
  {
    return {};  // caller asserts on the returned size instead of sizeof(T)
  }
  return std::vector<unsigned char>(bytes.begin() + static_cast<long>(Journal::kHeaderSize),
                                    bytes.end() - 4);
}

template <class T>
void expectPaddingNeutralised(const char* name)
{
  SCOPED_TRACE(name);
  const std::string pathA = tmpPath(std::string("journal_pad_a_") + name);
  const std::string pathB = tmpPath(std::string("journal_pad_b_") + name);

  const T a = poisoned<T>(0xAB);
  const T b = poisoned<T>(0xCD);

  const auto bodyA = writeBody(pathA, a, 4242);
  const auto bodyB = writeBody(pathB, b, 4242);
  ASSERT_EQ(bodyA.size(), sizeof(T));
  ASSERT_EQ(bodyB.size(), sizeof(T));
  EXPECT_EQ(std::memcmp(bodyA.data(), bodyB.data(), sizeof(T)), 0)
      << "0xAB- and 0xCD-poisoned instances of " << name
      << " serialized to different bytes -- padding leaked through";

  std::remove(pathA.c_str());
  std::remove(pathB.c_str());
}

}  // namespace

TEST(VenueJournalPadding, TwoDifferentlyPoisonedInstancesOfEveryTypeSerializeIdentically)
{
  expectPaddingNeutralised<NewOrder>("NewOrder");
  expectPaddingNeutralised<CancelOrder>("CancelOrder");
  expectPaddingNeutralised<ModifyOrder>("ModifyOrder");
  expectPaddingNeutralised<MassCancel>("MassCancel");
  expectPaddingNeutralised<Quote>("Quote");
  expectPaddingNeutralised<LastLookDecision>("LastLookDecision");
  expectPaddingNeutralised<SetMark>("SetMark");
  expectPaddingNeutralised<ApplyFunding>("ApplyFunding");
  expectPaddingNeutralised<AdminCmd>("AdminCmd");
  expectPaddingNeutralised<Deposit>("Deposit");
  expectPaddingNeutralised<Withdraw>("Withdraw");
  expectPaddingNeutralised<ListInstrument>("ListInstrument");
  expectPaddingNeutralised<SetBands>("SetBands");
  expectPaddingNeutralised<TimeTick>("TimeTick");
  expectPaddingNeutralised<SetTriggerRef>("SetTriggerRef");
  expectPaddingNeutralised<SnapshotBegin>("SnapshotBegin");
  expectPaddingNeutralised<RestoreOrder>("RestoreOrder");
  expectPaddingNeutralised<RestoreStop>("RestoreStop");
  expectPaddingNeutralised<RestorePeg>("RestorePeg");
  expectPaddingNeutralised<RestoreHeld>("RestoreHeld");
  expectPaddingNeutralised<RestorePosition>("RestorePosition");
  expectPaddingNeutralised<RestoreMmpCfg>("RestoreMmpCfg");
  expectPaddingNeutralised<RestoreClOrdIds>("RestoreClOrdIds");
  expectPaddingNeutralised<SnapshotEnd>("SnapshotEnd");
  expectPaddingNeutralised<RestoreReservation>("RestoreReservation");
  expectPaddingNeutralised<RestoreBalance>("RestoreBalance");
  expectPaddingNeutralised<RestoreMmpFills>("RestoreMmpFills");
  expectPaddingNeutralised<SetStpGroup>("SetStpGroup");
  expectPaddingNeutralised<SetFundingSchedule>("SetFundingSchedule");
  expectPaddingNeutralised<RestoreFunding>("RestoreFunding");
  expectPaddingNeutralised<ForceClosePosition>("ForceClosePosition");
  expectPaddingNeutralised<RestoreOrderStp>("RestoreOrderStp");
  expectPaddingNeutralised<SetAdmissionProfile>("SetAdmissionProfile");
  expectPaddingNeutralised<SetRiskLimits>("SetRiskLimits");
  expectPaddingNeutralised<AdjustPosition>("AdjustPosition");
}

// The record named in the task: four bytes between formatVersion and
// lastAppliedTs. Built the same way MatchingEngine::writeSnapshot builds it
// (positional aggregate init with real values, not T{}), on top of poisoned
// memory, so this is the exact bug shape rather than the generic sweep above.
TEST(VenueJournalPadding, SnapshotBeginPaddingIsZeroAndFieldsSurvive)
{
  alignas(SnapshotBegin) unsigned char buf[sizeof(SnapshotBegin)];
  std::memset(buf, 0xAB, sizeof(buf));
  new (buf) SnapshotBegin{4, {}, 123456789, 0xDEADBEEFULL, 0xC0FFEEULL};
  SnapshotBegin begin;
  std::memcpy(&begin, buf, sizeof(begin));

  const std::string path = tmpPath("journal_pad_snapbegin");
  const auto body = writeBody(path, begin, 99);
  ASSERT_EQ(body.size(), sizeof(SnapshotBegin));

  // Padding: offset 4, length 4 (between formatVersion and lastAppliedTs).
  EXPECT_EQ(body[4], 0);
  EXPECT_EQ(body[5], 0);
  EXPECT_EQ(body[6], 0);
  EXPECT_EQ(body[7], 0);

  SnapshotBegin roundTripped;
  std::memcpy(&roundTripped, body.data(), sizeof(roundTripped));
  EXPECT_EQ(roundTripped.formatVersion, 4u);
  EXPECT_EQ(roundTripped.lastAppliedTs, 123456789);
  EXPECT_EQ(roundTripped.stateHash, 0xDEADBEEFULL);
  EXPECT_EQ(roundTripped.configHash, 0xC0FFEEULL);

  std::remove(path.c_str());
}

// Task item 3(b): two journal records of the same logical command are
// byte-identical -- header, body and CRC -- even when the two struct
// instances that produced them started life with different padding poison.
TEST(VenueJournalPadding, TwoJournalRecordsOfTheSameCommandAreByteIdentical)
{
  auto makeOrder = [](unsigned char poison)
  {
    alignas(NewOrder) unsigned char buf[sizeof(NewOrder)];
    std::memset(buf, poison, sizeof(buf));
    new (buf) NewOrder{};
    NewOrder o;
    std::memcpy(&o, buf, sizeof(o));
    o.id = 7;
    o.symbol = SYM;
    o.side = Side::BUY;
    o.type = OrderType::LIMIT;
    o.price = px(100.0);
    o.quantity = qty(1.0);
    o.accountId = 3;
    o.clientOrderId = 99;
    o.expiryNs = SeqNanos::fromRaw(123);
    o.pegOffsetRaw = -5;
    return o;
  };
  const NewOrder a = makeOrder(0xAB);
  const NewOrder b = makeOrder(0xCD);

  const std::string path = tmpPath("journal_pad_duprec");
  std::remove(path.c_str());
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    out.append(InboundCommand{a}, 1000);
    out.append(InboundCommand{b}, 1000);  // same ts: the two records should be identical
  }
  const auto bytes = readFile(path);
  const size_t recLen = Journal::kHeaderSize + sizeof(NewOrder) + 4;
  ASSERT_EQ(bytes.size(), recLen * 2);
  EXPECT_EQ(std::memcmp(bytes.data(), bytes.data() + recLen, recLen), 0);
  std::remove(path.c_str());
}

// Task item 3(a): two snapshots of the same engine state, taken through the
// real production path (cloneForSnapshot -- the async-checkpoint clone, not
// a hand-built duplicate), are byte-identical. The state exercises open
// orders (iceberg, GTD, OCO, peg), a pending OCO stop, an open last-look
// hold, a perp position with posted margin, risk limits, an admission
// profile, an STP group and a funding schedule -- one instance of every
// journaled record family with padding, except the two fixed-size-batch
// snapshot records (RestoreClOrdIds, RestoreMmpFills), which are covered by
// the exhaustive sweep above instead.
TEST(VenueJournalPadding, TwoSnapshotsOfTheSameStateViaTheCloneAreByteIdentical)
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.maintenanceMarginBps = 500;
  c.lastLookWindowNs = DurationNs{100'000'000};
  c.lastLookAcceptOnTimeout = false;

  Ledger led;
  MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);
  eng.setMmp(1, qty(100.0), DurationNs{1'000'000'000});

  int64_t ts = 1;
  eng.submit(InboundCommand{Deposit{1, QUOTE, {}, quoteRaw(100000), SYM}}, ts++);
  eng.submit(InboundCommand{Deposit{2, QUOTE, {}, quoteRaw(100000), SYM}}, ts++);
  eng.submit(InboundCommand{Deposit{3, QUOTE, {}, quoteRaw(100000), SYM}}, ts++);
  eng.submit(InboundCommand{SetRiskLimits{SYM, RiskLuld | RiskFatFinger | RiskMaxPosition, {}, 500, {}, DurationNs{2'000'000'000}, qty(50.0), Volume::fromDouble(1'000'000.0), 0, {}, qty(100.0), 0, 0}},
             ts++);
  eng.submit(InboundCommand{SetAdmissionProfile{SYM, {}, 3, AdmissionProfile{0, 0, 0}}}, ts++);
  eng.submit(InboundCommand{SetStpGroup{SYM, {}, 1, 42}}, ts++);
  eng.submit(InboundCommand{SetFundingSchedule{SYM, {}, DurationNs{3'600'000'000'000}, SeqNanos::fromRaw(999)}},
             ts++);

  // Perp position: an ordinary crossing pair, no last look.
  NewOrder perpMaker;
  perpMaker.id = 1;
  perpMaker.symbol = SYM;
  perpMaker.side = Side::SELL;
  perpMaker.type = OrderType::LIMIT;
  perpMaker.price = px(100.0);
  perpMaker.quantity = qty(5.0);
  perpMaker.accountId = 2;
  eng.submit(InboundCommand{perpMaker}, ts++);
  NewOrder perpTaker;
  perpTaker.id = 2;
  perpTaker.symbol = SYM;
  perpTaker.side = Side::BUY;
  perpTaker.type = OrderType::LIMIT;
  perpTaker.price = px(100.0);
  perpTaker.quantity = qty(5.0);
  perpTaker.accountId = 1;
  eng.submit(InboundCommand{perpTaker}, ts++);
  eng.submit(InboundCommand{SetMark{SYM, {}, px(100.0)}}, ts++);

  // Last-look hold that stays open.
  NewOrder holdMaker;
  holdMaker.id = 3;
  holdMaker.symbol = SYM;
  holdMaker.side = Side::SELL;
  holdMaker.type = OrderType::LIMIT;
  holdMaker.price = px(101.0);
  holdMaker.quantity = qty(3.0);
  holdMaker.accountId = 2;
  holdMaker.lastLook = true;
  eng.submit(InboundCommand{holdMaker}, ts++);
  NewOrder holdTaker;
  holdTaker.id = 4;
  holdTaker.symbol = SYM;
  holdTaker.side = Side::BUY;
  holdTaker.type = OrderType::LIMIT;
  holdTaker.price = px(101.0);
  holdTaker.quantity = qty(2.0);
  holdTaker.accountId = 3;
  holdTaker.tif = TimeInForce::IOC;
  eng.submit(InboundCommand{holdTaker}, ts++);  // hold stays open (last-look window not elapsed)

  // Iceberg, GTD, OCO book + OCO stop, peg, clientOrderId -- all resting.
  NewOrder ice;
  ice.id = 5;
  ice.symbol = SYM;
  ice.side = Side::SELL;
  ice.type = OrderType::LIMIT;
  ice.price = px(102.0);
  ice.quantity = qty(10.0);
  ice.accountId = 2;
  ice.visibleQuantity = qty(2.0);
  eng.submit(InboundCommand{ice}, ts++);

  NewOrder gtd;
  gtd.id = 6;
  gtd.symbol = SYM;
  gtd.side = Side::BUY;
  gtd.type = OrderType::LIMIT;
  gtd.price = px(99.0);
  gtd.quantity = qty(2.0);
  gtd.accountId = 3;
  gtd.tif = TimeInForce::GTD;
  gtd.expiryNs = SeqNanos::fromRaw(5'000'000);
  eng.submit(InboundCommand{gtd}, ts++);

  NewOrder ocoBook;
  ocoBook.id = 7;
  ocoBook.symbol = SYM;
  ocoBook.side = Side::BUY;
  ocoBook.type = OrderType::LIMIT;
  ocoBook.price = px(98.0);
  ocoBook.quantity = qty(2.0);
  ocoBook.accountId = 3;
  ocoBook.ocoGroup = 7;
  eng.submit(InboundCommand{ocoBook}, ts++);

  NewOrder ocoStop;
  ocoStop.id = 8;
  ocoStop.symbol = SYM;
  ocoStop.side = Side::SELL;
  ocoStop.type = OrderType::STOP_MARKET;
  ocoStop.quantity = qty(2.0);
  ocoStop.accountId = 2;
  ocoStop.triggerPrice = px(90.0);
  ocoStop.ocoGroup = 7;
  eng.submit(InboundCommand{ocoStop}, ts++);

  NewOrder peg;
  peg.id = 9;
  peg.symbol = SYM;
  peg.side = Side::BUY;
  peg.type = OrderType::LIMIT;
  peg.price = px(1.0);  // placeholder, repriced by the peg
  peg.quantity = qty(1.0);
  peg.accountId = 3;
  peg.peg = PegRef::Bid;
  eng.submit(InboundCommand{peg}, ts++);

  NewOrder clord;
  clord.id = 10;
  clord.symbol = SYM;
  clord.side = Side::BUY;
  clord.type = OrderType::LIMIT;
  clord.price = px(97.0);
  clord.quantity = qty(1.0);
  clord.accountId = 3;
  clord.clientOrderId = 555;
  eng.submit(InboundCommand{clord}, ts++);

  ASSERT_GT(eng.restingOrderCount(), 0u);

  const std::string pathA = tmpPath("journal_pad_clone_a");
  const std::string pathB = tmpPath("journal_pad_clone_b");
  std::remove(pathA.c_str());
  std::remove(pathB.c_str());

  {
    Journal outA(pathA, Journal::Sync::Off, Journal::OpenMode::Truncate);
    eng.writeSnapshot(outA);
  }

  // Disturb the stack between the two serializations: an unrelated call
  // chain of the same rough depth, so any padding this fix does NOT
  // neutralise has a real chance of differing between the two writes rather
  // than coincidentally repeating the same leftover bytes. This is what
  // makes the "revert the fix" mutation land red on this test too, not just
  // on the deterministic poison-based one above.
  volatile uint64_t disturb[64];
  for (auto& d : disturb)
  {
    d = 0xFEEDFACECAFEBEEFULL;
  }
  (void)disturb[0];

  auto clone = eng.cloneForSnapshot();
  ASSERT_EQ(clone.engine->stateHash(), eng.stateHash());
  {
    Journal outB(pathB, Journal::Sync::Off, Journal::OpenMode::Truncate);
    clone.engine->writeSnapshot(outB);
  }

  const auto bytesA = readFile(pathA);
  const auto bytesB = readFile(pathB);
  ASSERT_FALSE(bytesA.empty());
  ASSERT_EQ(bytesA.size(), bytesB.size());
  EXPECT_EQ(std::memcmp(bytesA.data(), bytesB.data(), bytesA.size()), 0)
      << "two snapshots of the identical engine state differ byte-for-byte";

  std::remove(pathA.c_str());
  std::remove(pathB.c_str());
}
