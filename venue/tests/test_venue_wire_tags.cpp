/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/journal.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;

// The tag is the alternative's own, written down, not its position in the
// variant. That distinction is the whole point: while they were the same
// thing, reordering the variant re-read every old journal as DIFFERENT
// commands rather than refusing it.
TEST(WireTags, EveryAlternativeHasOneAndTheyAreUnique)
{
  EXPECT_EQ(std::size(kWireTag), std::variant_size_v<InboundCommand>);

  const std::set<uint8_t> unique(std::begin(kWireTag), std::end(kWireTag));
  EXPECT_EQ(unique.size(), std::size(kWireTag)) << "two alternatives share a tag";
}

// The tags on disk today. Pinned as a list because this IS the format: a
// change here is a change to every journal ever written, and it should have to
// be typed out deliberately rather than fall out of an edit somewhere else.
TEST(WireTags, TheTagsOnDiskArePinned)
{
  const std::vector<uint8_t> expected = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                         12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                                         24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35};
  const std::vector<uint8_t> actual(std::begin(kWireTag), std::end(kWireTag));
  EXPECT_EQ(actual, expected);
}

// A command's tag is what goes on the wire, and it comes back as the same
// alternative.
TEST(WireTags, ACommandRoundTripsUnderItsOwnTag)
{
  const std::string path = tmpPath("venue_wire_tags", ".bin");
  std::remove(path.c_str());

  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{TimeTick{SYM}}, 1);
    AdjustPosition a{};
    a.accountId = 5;
    a.symbol = SYM;
    a.qtyDeltaRaw = -7;
    j.append(InboundCommand{a}, 2);
    j.flush();
  }

  const auto records = Journal::loadTimed(path);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_NE(std::get_if<TimeTick>(&records[0].second), nullptr);
  const auto* back = std::get_if<AdjustPosition>(&records[1].second);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->qtyDeltaRaw, -7);

  std::remove(path.c_str());
}

// Snapshot-only records are recognised by tag, not by a range of variant
// positions. The range was the same conflation in another place: reordering
// the variant silently changed which records a client was allowed to send.
TEST(WireTags, SnapshotOnlyRecordsAreRecognisedByTag)
{
  EXPECT_TRUE(isSnapshotRecord(InboundCommand{SnapshotBegin{}}));
  EXPECT_TRUE(isSnapshotRecord(InboundCommand{RestoreOrder{}}));
  EXPECT_TRUE(isSnapshotRecord(InboundCommand{RestoreBalance{}}));
  EXPECT_TRUE(isSnapshotRecord(InboundCommand{RestoreFunding{}}));
  EXPECT_TRUE(isSnapshotRecord(InboundCommand{SnapshotEnd{}}));

  EXPECT_FALSE(isSnapshotRecord(InboundCommand{NewOrder{}}));
  EXPECT_FALSE(isSnapshotRecord(InboundCommand{TimeTick{}}));
  EXPECT_FALSE(isSnapshotRecord(InboundCommand{AdjustPosition{}}));
  EXPECT_FALSE(isSnapshotRecord(InboundCommand{SetRiskLimits{}}));

  // Every tag the predicate claims belongs to some alternative.
  for (const uint8_t t : kSnapshotOnlyTags)
  {
    EXPECT_NE(std::find(std::begin(kWireTag), std::end(kWireTag), t), std::end(kWireTag))
        << "snapshot-only tag " << static_cast<unsigned>(t) << " names no alternative";
  }
}

// The fingerprint is a property of the format, not of the variant's current
// shape: it is folded in tag order, so the same tags with the same body sizes
// give the same value however the alternatives are arranged in the type.
TEST(WireTags, TheFingerprintIsFoldedInTagOrder)
{
  // Recomputed here the way the header does it, from the same inputs but
  // deliberately visited in reverse -- a fold that depended on declaration
  // order would not survive this.
  struct Entry
  {
    uint8_t tag;
    size_t size;
  };
  std::vector<Entry> entries;
  const size_t sizes[] = {
      sizeof(NewOrder),
      sizeof(CancelOrder),
      sizeof(ModifyOrder),
      sizeof(MassCancel),
      sizeof(Quote),
      sizeof(LastLookDecision),
      sizeof(SetMark),
      sizeof(ApplyFunding),
      sizeof(AdminCmd),
      sizeof(Deposit),
      sizeof(Withdraw),
      sizeof(ListInstrument),
      sizeof(SetBands),
      sizeof(TimeTick),
      sizeof(SetTriggerRef),
      sizeof(SnapshotBegin),
      sizeof(RestoreOrder),
      sizeof(RestoreStop),
      sizeof(RestorePeg),
      sizeof(RestoreHeld),
      sizeof(RestorePosition),
      sizeof(RestoreMmpCfg),
      sizeof(RestoreClOrdIds),
      sizeof(SnapshotEnd),
      sizeof(RestoreReservation),
      sizeof(RestoreBalance),
      sizeof(RestoreMmpFills),
      sizeof(SetStpGroup),
      sizeof(SetFundingSchedule),
      sizeof(RestoreFunding),
      sizeof(ForceClosePosition),
      sizeof(RestoreOrderStp),
      sizeof(SetAdmissionProfile),
      sizeof(SetRiskLimits),
      sizeof(AdjustPosition),
      sizeof(QuoteLadder),
  };
  ASSERT_EQ(std::size(sizes), std::size(kWireTag));
  for (size_t i = std::size(sizes); i > 0; --i)
  {
    entries.push_back(Entry{kWireTag[i - 1], sizes[i - 1]});  // reverse order
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b)
            { return a.tag < b.tag; });

  uint64_t h = 1469598103934665603ULL;
  for (const Entry& e : entries)
  {
    h ^= static_cast<uint64_t>(e.tag);
    h *= 1099511628211ULL;
    h ^= static_cast<uint64_t>(e.size);
    h *= 1099511628211ULL;
  }

  EXPECT_EQ(h, bodyLayoutFingerprint())
      << "the fingerprint depends on the order the sizes were visited";
}

// The invariant that makes a reorder safe: the size the decoder expects for a
// tag is the size of the alternative that OWNS that tag. Move an alternative
// in the variant without moving its tag and this breaks -- which is the
// failure the whole change exists to turn from silent into loud.
//
// Demonstrated by hand as well as asserted here: a journal written before two
// alternatives were swapped in the variant reads back as the same commands
// afterwards. Under the old scheme, where the tag WAS the index, it read back
// as different ones.
TEST(WireTags, TheDecoderExpectsEachTagsOwnBodySize)
{
  const auto check = [](const InboundCommand& c, size_t expected)
  {
    const uint8_t tag = wireTagOf(c);
    EXPECT_EQ(Journal::expectedBodySizeForTag(tag), expected)
        << "tag " << static_cast<unsigned>(tag) << " decodes at the wrong size";
  };

  check(InboundCommand{NewOrder{}}, sizeof(NewOrder));
  check(InboundCommand{CancelOrder{}}, sizeof(CancelOrder));
  check(InboundCommand{TimeTick{}}, sizeof(TimeTick));
  check(InboundCommand{SnapshotBegin{}}, sizeof(SnapshotBegin));
  check(InboundCommand{RestoreClOrdIds{}}, sizeof(RestoreClOrdIds));
  check(InboundCommand{SetRiskLimits{}}, sizeof(SetRiskLimits));
  check(InboundCommand{AdjustPosition{}}, sizeof(AdjustPosition));
}

}  // namespace
