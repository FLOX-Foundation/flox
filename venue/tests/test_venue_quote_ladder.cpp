/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * QuoteLadder: one command for a maker's whole set of levels on a symbol.
 *
 * The claim the command is worth having only if it holds: a ladder does what
 * the Quotes it replaces did. Not "something equivalent", not "the same
 * orders end up resting" -- the same legs, in the same order, under the same
 * ids, publishing the same events, in the same sequence. So the shape of
 * every test here is a DIFFERENTIAL one: two engines, identical
 * configuration, one fed a ladder and the other fed the Quotes that ladder
 * stands for, compared on the engine's own state digest and on the stream
 * digest (event_hash.h) that the golden corpus is compared on.
 *
 * A test that only compared the resting book would pass on a ladder that
 * published its accepts in the wrong order, or dropped an OrderCanceled for a
 * level it took down. The stream digest is what makes the comparison
 * byte-for-byte rather than approximate: every field of every event, in
 * order.
 *
 * The mutation this file exists to catch is named in the task: a ladder that
 * skips a level. SkippingALevelIsNotTheSameLadder is that mutation applied to
 * the REFERENCE side, where a test can hold it: the same ladder against one
 * fewer Quote must not agree. Applied to the engine instead -- making the
 * rung loop stop one short -- it fails the equality tests above it, which is
 * the same statement from the other side.
 */
#include "flox-venue/engine/quote.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sbe_order_entry_codec.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
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
constexpr uint64_t ACCT = 7;
constexpr OrderId BID_BASE = 1000;
constexpr OrderId ASK_BASE = 2000;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
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

// One engine, its event stream folded exactly the way the golden corpus folds
// it, plus the raw events so a failure can say WHICH one diverged rather than
// only that a digest did.
struct Venue
{
  std::vector<OutboundEvent> events;
  uint64_t stream{1469598103934665603ULL};
  MatchingEngine<MatchingBook> eng;
  int64_t ts{1};

  Venue()
      : eng(cfg(), [this](const OutboundEvent& e)
            {
              events.push_back(e);
              stream = hashEvent(stream, e); })
  {
  }
  Venue(const Venue&) = delete;
  Venue& operator=(const Venue&) = delete;

  // Both sides of every comparison here submit at the SAME sequencer time,
  // because that is what the command claims: one slot instead of K. A
  // reference that walked the clock K times would be a different run before
  // the first leg was built -- engine time reaches the state digest through
  // the clOrdId window and every deadline the engine keeps.
  void push(const InboundCommand& c) { eng.submit(c, ts); }
  void step() { ++ts; }
};

// A ladder whose rungs step away from a 100.00 mid, one tick per level, with
// sizes that differ per level so a rung read from the wrong slot is visible.
QuoteLadder ladder(uint8_t levels)
{
  QuoteLadder l;
  l.accountId = ACCT;
  l.symbol = SYM;
  l.bidIdBase = BID_BASE;
  l.askIdBase = ASK_BASE;
  l.levels = levels;
  l.postOnly = true;
  l.tif = TimeInForce::GTC;
  for (uint8_t i = 0; i < levels && i < kQuoteLadderLevels; ++i)
  {
    l.level[i].bidPrice = px(99.99 - 0.01 * i);
    l.level[i].bidQty = qty(1.0 + i);
    l.level[i].askPrice = px(100.01 + 0.01 * i);
    l.level[i].askQty = qty(2.0 + i);
  }
  return l;
}

// The Quotes a ladder stands for, built the way a submitter would have built
// them before the command existed: one per slot of the id block, the live
// ones carrying the rung and the rest carrying nothing on either side.
std::vector<Quote> asQuotes(const QuoteLadder& l, uint8_t slots = kQuoteLadderLevels)
{
  std::vector<Quote> v;
  for (uint8_t i = 0; i < slots; ++i)
  {
    Quote q;
    q.bidId = l.bidIdBase + i;
    q.askId = l.askIdBase + i;
    q.symbol = l.symbol;
    if (i < quoteLadderLiveLevels(l))
    {
      q.bidPrice = l.level[i].bidPrice;
      q.bidQty = l.level[i].bidQty;
      q.askPrice = l.level[i].askPrice;
      q.askQty = l.level[i].askQty;
    }
    q.accountId = l.accountId;
    q.stp = l.stp;
    q.lastLook = l.lastLook;
    q.postOnly = l.postOnly;
    q.reduceOnly = l.reduceOnly;
    q.tif = l.tif;
    q.visibleQuantity = l.visibleQuantity;
    q.expiryNs = l.expiryNs;
    q.clientOrderId = l.clientOrderId;
    v.push_back(q);
  }
  return v;
}

void expectSameStream(const Venue& a, const Venue& b)
{
  ASSERT_EQ(a.events.size(), b.events.size()) << "different number of events";
  for (size_t i = 0; i < a.events.size(); ++i)
  {
    uint64_t ha = 1469598103934665603ULL;
    uint64_t hb = ha;
    EXPECT_EQ(hashEvent(ha, a.events[i]), hashEvent(hb, b.events[i]))
        << "event " << i << " differs";
  }
  EXPECT_EQ(a.stream, b.stream);
  EXPECT_EQ(a.eng.stateHash(), b.eng.stateHash());
  EXPECT_EQ(a.eng.restingOrderCount(), b.eng.restingOrderCount());
}

}  // namespace

// The whole claim, on a full block: one ladder, kQuoteLadderLevels Quotes,
// nothing to tell them apart from outside the engine.
TEST(VenueQuoteLadder, AFullLadderIsTheQuotesItStandsFor)
{
  const QuoteLadder l = ladder(kQuoteLadderLevels);

  Venue viaLadder;
  viaLadder.push(InboundCommand{l});

  Venue viaQuotes;
  for (const Quote& q : asQuotes(l))
  {
    viaQuotes.push(InboundCommand{q});
  }

  EXPECT_EQ(viaLadder.eng.restingOrderCount(), 2U * kQuoteLadderLevels);
  expectSameStream(viaLadder, viaQuotes);
}

// The case the product actually sends: fewer rungs than the block holds. The
// slots past the live ones are quotes with no size on either side, so on an
// empty book they cancel nothing and publish nothing -- the ladder's stream
// is exactly the five levels it named.
TEST(VenueQuoteLadder, AShortLadderIsTheQuotesItStandsFor)
{
  const QuoteLadder l = ladder(5);

  Venue viaLadder;
  viaLadder.push(InboundCommand{l});

  Venue viaQuotes;
  for (const Quote& q : asQuotes(l))
  {
    viaQuotes.push(InboundCommand{q});
  }

  EXPECT_EQ(viaLadder.eng.restingOrderCount(), 10U);
  expectSameStream(viaLadder, viaQuotes);
}

// A maker that publishes five levels and then three: the two rungs it stopped
// naming leave the book, because the ladder owns the id block and can name
// the levels the submitter no longer does. This is the behaviour a set of
// independent Quotes could not have -- and it is still, event for event, what
// the Quotes for the same id block would have produced.
TEST(VenueQuoteLadder, AShorterLadderTakesDownTheLevelsItDropped)
{
  const QuoteLadder wide = ladder(5);
  const QuoteLadder narrow = ladder(3);

  Venue viaLadder;
  viaLadder.push(InboundCommand{wide});
  ASSERT_EQ(viaLadder.eng.restingOrderCount(), 10U);
  viaLadder.step();
  viaLadder.push(InboundCommand{narrow});
  EXPECT_EQ(viaLadder.eng.restingOrderCount(), 6U);

  Venue viaQuotes;
  for (const Quote& q : asQuotes(wide))
  {
    viaQuotes.push(InboundCommand{q});
  }
  viaQuotes.step();
  for (const Quote& q : asQuotes(narrow))
  {
    viaQuotes.push(InboundCommand{q});
  }

  expectSameStream(viaLadder, viaQuotes);
}

// A ladder that reprices in place: every level cancels and re-posts, so the
// stream is two events per side per level and their ORDER is the thing at
// stake. A rung loop that ran the cancels of the whole block before the
// posts would end in the same book and fail here.
TEST(VenueQuoteLadder, ARepricedLadderIsTheQuotesItStandsFor)
{
  QuoteLadder first = ladder(4);
  QuoteLadder second = ladder(4);
  for (uint8_t i = 0; i < 4; ++i)
  {
    second.level[i].bidPrice = px(99.50 - 0.01 * i);
    second.level[i].askPrice = px(100.50 + 0.01 * i);
    second.level[i].bidQty = qty(3.0 + i);
  }

  Venue viaLadder;
  viaLadder.push(InboundCommand{first});
  viaLadder.step();
  viaLadder.push(InboundCommand{second});

  Venue viaQuotes;
  for (const Quote& q : asQuotes(first))
  {
    viaQuotes.push(InboundCommand{q});
  }
  viaQuotes.step();
  for (const Quote& q : asQuotes(second))
  {
    viaQuotes.push(InboundCommand{q});
  }

  expectSameStream(viaLadder, viaQuotes);
}

// The mutation, held where a test can hold it: the same ladder against a
// reference that skips a level must NOT agree. If it did, every equality
// above would be worth nothing -- they would be comparing something that
// cannot tell a missing rung from a present one.
TEST(VenueQuoteLadder, SkippingALevelIsNotTheSameLadder)
{
  const QuoteLadder l = ladder(5);

  Venue viaLadder;
  viaLadder.push(InboundCommand{l});

  Venue skipped;
  std::vector<Quote> qs = asQuotes(l);
  qs.erase(qs.begin() + 2);  // level 2 never sent
  for (const Quote& q : qs)
  {
    skipped.push(InboundCommand{q});
  }

  EXPECT_NE(viaLadder.stream, skipped.stream);
  EXPECT_NE(viaLadder.eng.stateHash(), skipped.eng.stateHash());
  EXPECT_NE(viaLadder.eng.restingOrderCount(), skipped.eng.restingOrderCount());
}

// The rung the wire could not carry. `levels` past the end of the block is
// clamped rather than refused -- and clamped by the same helper the journal
// writer asks, which is what keeps a live run and its replay from disagreeing
// about how long the record was.
TEST(VenueQuoteLadder, LevelsPastTheBlockAreClamped)
{
  QuoteLadder l = ladder(kQuoteLadderLevels);
  l.levels = 200;
  EXPECT_EQ(quoteLadderLiveLevels(l), kQuoteLadderLevels);
  EXPECT_EQ(quoteLadderBodySize(l), sizeof(QuoteLadder));

  Venue viaLadder;
  viaLadder.push(InboundCommand{l});
  EXPECT_EQ(viaLadder.eng.restingOrderCount(), 2U * kQuoteLadderLevels);
}

// One submission, one name, one dedup slot -- the rule a Quote already
// applies to its two legs, one level up. Every leg carries the ladder's name,
// and a resend of that name is refused once, as a whole, leaving the resting
// ladder alone.
TEST(VenueQuoteLadder, TheLadderIsOneNameAndOneDedupSlot)
{
  QuoteLadder l = ladder(3);
  l.clientOrderId = 4242;

  Venue r;
  r.push(InboundCommand{l});
  ASSERT_EQ(r.eng.restingOrderCount(), 6U);

  size_t accepted = 0;
  for (const OutboundEvent& e : r.events)
  {
    if (const auto* a = std::get_if<OrderAccepted>(&e))
    {
      EXPECT_EQ(a->clientOrderId, 4242U);
      ++accepted;
    }
    EXPECT_EQ(std::get_if<OrderRejected>(&e), nullptr);
  }
  EXPECT_EQ(accepted, 6U);

  const size_t before = r.events.size();
  r.step();
  r.push(InboundCommand{l});
  ASSERT_EQ(r.events.size(), before + 1);
  const auto* rej = std::get_if<OrderRejected>(&r.events.back());
  ASSERT_NE(rej, nullptr);
  EXPECT_EQ(rej->reason, RejectReason::DuplicateClientOrderId);
  EXPECT_EQ(rej->id, BID_BASE);              // the ladder's first bid id names the refusal
  EXPECT_EQ(r.eng.restingOrderCount(), 6U);  // the resting ladder is untouched
}

// ---- the journal ---------------------------------------------------------

// A ladder record is as long as the rungs it names, and comes back as the
// ladder that was written. The length is the point of the record, so it is
// asserted rather than merely round-tripped.
TEST(VenueQuoteLadderJournal, ARecordIsAsLongAsTheRungsItNames)
{
  const std::string path = tmpPath("venue_quote_ladder", ".bin");
  std::remove(path.c_str());

  const QuoteLadder five = ladder(5);
  const QuoteLadder full = ladder(kQuoteLadderLevels);
  const QuoteLadder none = ladder(0);
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{five}, 11);
    j.append(InboundCommand{full}, 22);
    j.append(InboundCommand{none}, 33);
    j.flush();
  }

  EXPECT_EQ(quoteLadderBodySize(five), kQuoteLadderHeadSize + 5 * sizeof(QuoteLadderLevel));
  EXPECT_EQ(quoteLadderBodySize(full), sizeof(QuoteLadder));
  EXPECT_EQ(quoteLadderBodySize(none), kQuoteLadderHeadSize);

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  ASSERT_TRUE(in);
  const size_t frame = Journal::kHeaderSize + sizeof(uint32_t);  // header + crc
  EXPECT_EQ(static_cast<size_t>(in.tellg()),
            3 * frame + quoteLadderBodySize(five) + quoteLadderBodySize(full) +
                quoteLadderBodySize(none));

  const auto back = Journal::loadTimed(path);
  ASSERT_EQ(back.size(), 3U);
  const auto* r0 = std::get_if<QuoteLadder>(&back[0].second);
  ASSERT_NE(r0, nullptr);
  EXPECT_EQ(back[0].first, 11);
  EXPECT_EQ(r0->levels, 5);
  EXPECT_EQ(r0->bidIdBase, BID_BASE);
  EXPECT_EQ(r0->askIdBase, ASK_BASE);
  EXPECT_EQ(r0->accountId, ACCT);
  EXPECT_TRUE(r0->postOnly);
  for (uint8_t i = 0; i < 5; ++i)
  {
    EXPECT_EQ(r0->level[i].bidPrice.raw(), five.level[i].bidPrice.raw()) << "level " << +i;
    EXPECT_EQ(r0->level[i].bidQty.raw(), five.level[i].bidQty.raw()) << "level " << +i;
    EXPECT_EQ(r0->level[i].askPrice.raw(), five.level[i].askPrice.raw()) << "level " << +i;
    EXPECT_EQ(r0->level[i].askQty.raw(), five.level[i].askQty.raw()) << "level " << +i;
  }
  // The rungs the record did not carry come back as the zeros they were.
  for (uint8_t i = 5; i < kQuoteLadderLevels; ++i)
  {
    EXPECT_EQ(r0->level[i].bidQty.raw(), 0) << "level " << +i;
    EXPECT_EQ(r0->level[i].askQty.raw(), 0) << "level " << +i;
  }
  const auto* r1 = std::get_if<QuoteLadder>(&back[1].second);
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(r1->levels, kQuoteLadderLevels);
  const auto* r2 = std::get_if<QuoteLadder>(&back[2].second);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(r2->levels, 0);

  std::remove(path.c_str());
}

// Five levels as one record against five levels as five Quotes, asserted
// rather than only reported by the benchmark.
//
// The two counts are derived from the types, not from numbers somebody
// measured once: what is being pinned is the SHAPE of the record -- a frame
// plus a head plus the rungs it names, against five frames plus five whole
// Quotes -- so a field added to either side moves the assertion rather than
// quietly eating the margin.
//
// The ratio is stated per layout, because the layout is what it depends on.
// The shipped one clears 2x with room (242 against 570). A scale-checked
// build is a different format and not a debugging variant of the same one
// (docs/venue/runtime.md): it widens every Price and Quantity to 16 bytes,
// which grows a rung faster than it grows a Quote's fixed part, so five
// levels land at 1.88x there and 2x arrives at eight. No venue writes that
// layout, so the shipped number is the one the claim is about -- and the
// weaker bound is still asserted rather than skipped, so a regression in the
// checked layout is not invisible.
TEST(VenueQuoteLadderJournal, FiveLevelsCostLessThanHalfOfFiveQuotes)
{
  const std::string ladderPath = tmpPath("venue_quote_ladder_bytes_l", ".bin");
  const std::string quotePath = tmpPath("venue_quote_ladder_bytes_q", ".bin");
  std::remove(ladderPath.c_str());
  std::remove(quotePath.c_str());

  const QuoteLadder l = ladder(5);
  uint64_t ladderBytes = 0;
  uint64_t quoteBytes = 0;
  {
    Journal j(ladderPath, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{l}, 1);
    j.flush();
    ladderBytes = j.bytes();
  }
  {
    Journal j(quotePath, Journal::Sync::Off, Journal::OpenMode::Truncate);
    int64_t ts = 1;
    for (const Quote& q : asQuotes(l, 5))
    {
      j.append(InboundCommand{q}, ts++);
    }
    j.flush();
    quoteBytes = j.bytes();
  }

  const uint64_t frame = Journal::kHeaderSize + sizeof(uint32_t);  // header + crc
  EXPECT_EQ(ladderBytes, frame + kQuoteLadderHeadSize + 5 * sizeof(QuoteLadderLevel));
  EXPECT_EQ(quoteBytes, 5 * (frame + sizeof(Quote)));

#if FLOX_SCALE_CHECKS
  EXPECT_LE(ladderBytes * 9, quoteBytes * 5)  // at least 1.8x
      << "ladder " << ladderBytes << " bytes, five quotes " << quoteBytes;
#else
  EXPECT_LE(ladderBytes * 2, quoteBytes)
      << "ladder " << ladderBytes << " bytes, five quotes " << quoteBytes;
#endif

  std::remove(ladderPath.c_str());
  std::remove(quotePath.c_str());
}

// A ladder journal replays through the engine as the same ladder: the record
// is the command, and recovery is one submit rather than K.
TEST(VenueQuoteLadderJournal, AReplayedLadderRebuildsTheSameBook)
{
  const std::string path = tmpPath("venue_quote_ladder_replay", ".bin");
  std::remove(path.c_str());

  const QuoteLadder wide = ladder(5);
  const QuoteLadder narrow = ladder(3);

  Venue live;
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{wide}, live.ts);
    live.push(InboundCommand{wide});
    live.step();
    j.append(InboundCommand{narrow}, live.ts);
    live.push(InboundCommand{narrow});
    j.flush();
  }

  Venue replayed;
  for (const auto& [ts, cmd] : Journal::loadTimed(path))
  {
    replayed.eng.submit(cmd, ts);
  }
  EXPECT_EQ(replayed.eng.stateHash(), live.eng.stateHash());
  EXPECT_EQ(replayed.stream, live.stream);

  std::remove(path.c_str());
}

// Records this build does NOT write, framed the way a foreign build would
// have framed them: a ladder body that is not a whole number of rungs, and
// one whose length disagrees with its own `levels` byte. Neither is damage in
// transit -- both carry a valid crc -- so neither can be read as a short
// ladder: the loader returns the intact prefix and stops.
TEST(VenueQuoteLadderJournal, ABodyThatDisagreesWithItsLevelsStopsTheLoad)
{
  const auto write = [](const std::string& path, uint32_t len, uint8_t levelsByte)
  {
    // A good record first, so "stopped" is distinguishable from "read nothing".
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{TimeTick{SYM}}, 1);
    j.flush();

    QuoteLadder l = ladder(5);
    l.levels = levelsByte;
    std::vector<uint8_t> rec;
    const auto put = [&rec](const void* p, size_t n)
    {
      const auto* b = static_cast<const uint8_t*>(p);
      rec.insert(rec.end(), b, b + n);
    };
    const int64_t ts = 2;
    const uint8_t stamp = kRecordStamp;
    const uint8_t tag = kQuoteLadderWireTag;
    put(&ts, sizeof(ts));
    put(&stamp, sizeof(stamp));
    put(&tag, sizeof(tag));
    put(&len, sizeof(len));
    put(&l, len);
    const uint32_t crc = flox::util::Crc32::compute(rec.data(), rec.size());
    put(&crc, sizeof(crc));

    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(rec.data()), static_cast<long>(rec.size()));
  };

  const std::string ragged = tmpPath("venue_quote_ladder_ragged", ".bin");
  std::remove(ragged.c_str());
  write(ragged, static_cast<uint32_t>(kQuoteLadderHeadSize + 5 * sizeof(QuoteLadderLevel) - 8), 5);
  EXPECT_EQ(Journal::loadTimed(ragged).size(), 1U) << "a partial rung was read as a rung";
  std::remove(ragged.c_str());

  const std::string lying = tmpPath("venue_quote_ladder_lying", ".bin");
  std::remove(lying.c_str());
  write(lying, static_cast<uint32_t>(kQuoteLadderHeadSize + 5 * sizeof(QuoteLadderLevel)), 3);
  EXPECT_EQ(Journal::loadTimed(lying).size(), 1U) << "a record was read at a length it denies";
  std::remove(lying.c_str());
}

// The bump this command needed, from the other side: a file in the version
// pair that came before it is refused BY NAME rather than decoded at offsets
// this build does not have. A build without QuoteLadder has no type for tag
// 35 at all, so reading such a file backwards would be the worse failure --
// a prefix of the history returned as though it were all of it.
TEST(VenueQuoteLadderJournal, TheVersionBeforeTheLadderIsRefusedByName)
{
  const std::string path = tmpPath("venue_quote_ladder_oldver", ".bin");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{TimeTick{SYM}}, 1);
    j.flush();
  }
  // Derived from kRecordVersion, never a literal: the pair moves by two, so
  // the version this build replaced is the one two below it.
  const uint8_t previous = static_cast<uint8_t>(kRecordVersion - 2);
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f);
    f.seekp(8);  // the stamp byte
    const uint8_t stamp = static_cast<uint8_t>(kVersionedMark | previous);
    f.write(reinterpret_cast<const char*>(&stamp), 1);
  }

  try
  {
    Journal::loadTimed(path);
    ADD_FAILURE() << "a file in the previous version was decoded";
  }
  catch (const JournalFormatError& e)
  {
    const std::string what = e.what();
    EXPECT_NE(what.find(std::to_string(static_cast<unsigned>(previous))), std::string::npos)
        << what;
    EXPECT_NE(what.find(std::to_string(static_cast<unsigned>(kRecordVersion))), std::string::npos)
        << what;
  }
  std::remove(path.c_str());
}

// The other half of the bump: adding an alternative moved no existing record.
// A journal holding only the commands that existed before the ladder, written
// by this build, reads back as exactly those commands -- the new tag took a
// number nobody had, and no tag's body changed length.
TEST(VenueQuoteLadderJournal, RecordsWrittenWithoutALadderAreUnchanged)
{
  const std::string path = tmpPath("venue_quote_ladder_legacy", ".bin");
  std::remove(path.c_str());

  Quote q;
  q.bidId = 1;
  q.askId = 2;
  q.symbol = SYM;
  q.bidPrice = px(99.0);
  q.bidQty = qty(1.0);
  q.askPrice = px(101.0);
  q.askQty = qty(1.0);
  q.accountId = ACCT;

  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{q}, 1);
    j.append(InboundCommand{MassCancel{ACCT, SYM}}, 2);
    j.append(InboundCommand{TimeTick{SYM}}, 3);
    j.flush();
  }

  const auto back = Journal::loadTimed(path);
  ASSERT_EQ(back.size(), 3U);
  const auto* backQuote = std::get_if<Quote>(&back[0].second);
  ASSERT_NE(backQuote, nullptr);
  EXPECT_EQ(backQuote->bidId, 1U);
  EXPECT_EQ(backQuote->askQty.raw(), qty(1.0).raw());
  EXPECT_NE(std::get_if<MassCancel>(&back[1].second), nullptr);
  EXPECT_NE(std::get_if<TimeTick>(&back[2].second), nullptr);
  EXPECT_EQ(Journal::expectedBodySizeForTag(wireTagOf(InboundCommand{q})), sizeof(Quote));

  std::remove(path.c_str());
}

// ---- the order-entry wire ------------------------------------------------

// The ladder has a wire form, and the form is the command: encode, decode,
// and the venue is holding the ladder the client sent. Asserted through the
// ENGINE rather than field by field, because the only property that matters
// is the one the rest of this file is about -- a decoded ladder does what the
// ladder it was encoded from does.
TEST(VenueQuoteLadderWire, ALadderSurvivesTheOrderEntryWire)
{
  QuoteLadder l = ladder(5);
  l.clientOrderId = 88;
  l.stp = STPMode::CancelNewest;
  l.visibleQuantity = qty(0.5);
  l.expiryNs = SeqNanos::fromRaw(5'000'000);

  std::vector<uint8_t> frame;
  SbeOrderEntryCodec::encode(InboundCommand{l}, frame);
  EXPECT_EQ(SbeOrderEntryCodec::templateId(frame.data(), frame.size()),
            static_cast<uint16_t>(SbeOrderEntryCodec::InTmpl::QuoteLadder));

  const auto back = SbeOrderEntryCodec::decode(frame.data(), frame.size());
  ASSERT_TRUE(back.has_value());
  const auto* decoded = std::get_if<QuoteLadder>(&*back);
  ASSERT_NE(decoded, nullptr);

  Venue viaWire;
  viaWire.push(*back);
  Venue direct;
  direct.push(InboundCommand{l});
  expectSameStream(viaWire, direct);
}

// A count the block cannot hold is clamped on the way in, the same answer
// the engine and the journal give it -- a frame carries exactly the rungs it
// carries, so there is nothing a larger count could name.
TEST(VenueQuoteLadderWire, ACountPastTheBlockIsClampedOnDecode)
{
  QuoteLadder l = ladder(kQuoteLadderLevels);
  std::vector<uint8_t> frame;
  SbeOrderEntryCodec::encode(InboundCommand{l}, frame);
  ASSERT_GT(frame.size(), 61U);
  frame[8 + 52] = 250;  // the levels byte of the root block

  const auto back = SbeOrderEntryCodec::decode(frame.data(), frame.size());
  ASSERT_TRUE(back.has_value());
  const auto* decoded = std::get_if<QuoteLadder>(&*back);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->levels, kQuoteLadderLevels);
}

// A truncated ladder frame decodes to nothing rather than to a ladder with
// rungs read off whatever followed the buffer.
TEST(VenueQuoteLadderWire, ATruncatedLadderFrameIsRefused)
{
  QuoteLadder l = ladder(4);
  std::vector<uint8_t> frame;
  SbeOrderEntryCodec::encode(InboundCommand{l}, frame);
  frame.resize(frame.size() - 1);
  EXPECT_FALSE(SbeOrderEntryCodec::decode(frame.data(), frame.size()).has_value());
}
