/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * What a damaged journal costs.
 *
 * journal.h:19-22 states the promise these tests hold the loader to: "A torn
 * tail (a record whose bytes are not fully present) or a corrupted record is
 * DETECTED on load: the loader returns the largest intact prefix and never
 * materialises a partial/garbage command." docs/venue/runtime.md:229-231 says
 * the same from the other side -- "a torn tail is the expected shape of a
 * crash and the prefix ahead of it is sound, whereas a foreign version means
 * every byte after the header was laid out by rules this build does not have".
 *
 * Both sentences separate DAMAGE from a FOREIGN FORMAT, and the separator is
 * the crc: a record whose crc does not cover its own header was damaged after
 * it was written, whatever its stamp byte now reads. The loader decides the
 * other way round -- it tests the stamp, and the tag, before it has looked at
 * the crc -- so a single flipped bit in the last record's header is refused as
 * a foreign format and the whole journal goes with it.
 *
 * A file written by a genuinely older build is a different thing and must stay
 * refused by name; the green control below is the record that says so with a
 * crc that passes.
 */
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr size_t kGood = 10;

InboundCommand tick() { return InboundCommand{TimeTick{SYM}}; }

// One TimeTick record on disk: header + body + crc, the same for every one of
// them, which is what lets the tests index into the file by record number.
constexpr size_t kRecordSize = Journal::kHeaderSize + sizeof(TimeTick) + sizeof(uint32_t);

std::vector<uint8_t> readAll(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

void writeAll(const std::string& path, const std::vector<uint8_t>& bytes)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// A journal of kGood well-formed records, written by this build.
std::string goodJournal(const std::string& stem)
{
  const std::string path = tmpPath(stem, ".bin");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    for (size_t i = 0; i < kGood; ++i)
    {
      j.append(tick(), static_cast<int64_t>(i) + 1);
    }
    j.flush();
  }
  return path;
}

// Damage one byte of record `idx`, `at` bytes into it. Nothing else is
// touched, so the record's crc no longer covers the bytes on disk -- which is
// exactly what bit rot and a half-written page look like.
std::string damagedCopy(const std::string& stem, const std::vector<uint8_t>& src, size_t idx,
                        size_t at)
{
  auto bytes = src;
  bytes[idx * kRecordSize + at] ^= 0xFF;
  const std::string path = tmpPath(stem, ".bin");
  writeAll(path, bytes);
  return path;
}

// The stamp byte of a record, by its offset in the framing: [ts:8][stamp:1].
constexpr size_t kStampByte = 8;

// A record this build cannot read because it was written by a build that
// numbered the format differently -- and whose crc PASSES, so nothing about it
// looks damaged. kRecordVersion moves by two per format change, so
// kRecordVersion - 2 is the pair immediately before this one.
void appendForeignVersionRecord(const std::string& path)
{
  std::vector<uint8_t> rec;
  const auto put = [&rec](const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    rec.insert(rec.end(), b, b + n);
  };
  const int64_t ts = 7777;
  const uint8_t stamp = static_cast<uint8_t>(kVersionedMark | (kRecordVersion - 2));
  const uint8_t tag = wireTagOf(tick());
  const uint32_t len = static_cast<uint32_t>(sizeof(TimeTick));
  const TimeTick body{SYM};
  put(&ts, sizeof ts);
  put(&stamp, sizeof stamp);
  put(&tag, sizeof tag);
  put(&len, sizeof len);
  put(&body, sizeof body);
  const uint32_t crc = flox::util::Crc32::compute(rec.data(), rec.size());
  put(&crc, sizeof crc);

  std::FILE* f = std::fopen(path.c_str(), "ab");
  ASSERT_NE(f, nullptr);
  std::fwrite(rec.data(), 1, rec.size(), f);
  std::fclose(f);
}

// ---------------------------------------------------------------------------
// The reporting surface these tests need and the tree does not have yet.
//
// needs: enum class Journal::Tail : uint8_t { Intact, Torn, Corrupt };
// needs: struct Journal::LoadReport {
//          std::vector<std::pair<int64_t, InboundCommand>> records;
//          Tail tail;             // Intact: the read ended on a record boundary
//                                 // Torn:   the damaged/short record is the LAST
//                                 //         thing in the file
//                                 // Corrupt: whole bytes follow the damaged
//                                 //         record, so this is not a crash tail
//          uint64_t stopOffset;   // byte offset at which the first unrecovered
//                                 // record begins (== file size when Intact)
//        };
// needs: static LoadReport Journal::loadReported(const std::string& path);
//
// loadTimed keeps its signature and its throw-by-name behaviour for a foreign
// version and an unknown tag; loadReported is the same read with the stop
// described instead of implied. Detected by SFINAE so this file builds against
// the tree as it stands and the assertion, not the compiler, is what fails.
template <class J, class = void>
struct HasLoadReported : std::false_type
{
};
template <class J>
struct HasLoadReported<J, std::void_t<decltype(J::loadReported(std::string{}))>> : std::true_type
{
};

}  // namespace

// ---------------------------------------------------------------------------
// (1) One flipped byte in the last record's header must cost that record, not
// the journal. The stamp byte is the worst case because it is the field the
// loader tests first, before the crc that would have said "damaged".

TEST(VenueJournalTornTail, ACorruptStampByteOnTheLastRecordKeepsTheIntactPrefix)
{
  const std::string src = goodJournal("venue_torn_stamp_src");
  const auto bytes = readAll(src);
  ASSERT_EQ(bytes.size(), kRecordSize * kGood);
  ASSERT_EQ(Journal::loadTimed(src).size(), kGood);

  const std::string path = damagedCopy("venue_torn_stamp", bytes, kGood - 1, kStampByte);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); })
      << "a damaged tail is not a foreign format: the crc, not the stamp, says which it is";
  EXPECT_EQ(records.size(), kGood - 1);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// The tag byte sits next to the stamp and is read on the same pass, before the
// crc. A flipped tag that lands on no alternative is refused as a foreign
// build's command; a flipped tag that lands on another alternative is decoded
// as that command if its size happens to fit. Either way the record is damaged
// and the crc knows it.
TEST(VenueJournalTornTail, ACorruptTagByteOnTheLastRecordKeepsTheIntactPrefix)
{
  const std::string src = goodJournal("venue_torn_tag_src");
  const auto bytes = readAll(src);
  const std::string path = damagedCopy("venue_torn_tag", bytes, kGood - 1, kStampByte + 1);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); });
  EXPECT_EQ(records.size(), kGood - 1);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// (2) The same file through the path that matters: a shard recovering from it.
// SequencedShard::recover does not catch JournalFormatError, so today the
// throw comes out of start() and the shard does not exist -- the whole history
// lost to one bit, which is the outcome the torn-tail machinery was written to
// prevent.

TEST(VenueJournalTornTail, AShardStartsOnAJournalWhoseLastRecordHasACorruptStamp)
{
  const std::string src = goodJournal("venue_torn_shard_src");
  const auto bytes = readAll(src);
  const std::string path = damagedCopy("venue_torn_shard", bytes, kGood - 1, kStampByte);

  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);

  auto shard = std::make_unique<SequencedShard<>>(c, path, MatchingBook{}, Journal::Sync::Off);
  shard->setOwnThreads(false);
  EXPECT_NO_THROW({ shard->start(); })
      << "one damaged byte in the tail must not stop the shard from starting";
  EXPECT_TRUE(shard->ready());
  EXPECT_EQ(shard->recoveredCommands(), kGood - 1);
  shard->stop();
  shard.reset();

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// (3) Reported, not implied. "Returned nine records" and "returned nine
// records and the tenth was torn" are different answers, and only the second
// one lets an operator tell a clean shutdown from a crash without diffing file
// sizes by hand.

namespace
{
template <class J>
void expectTornTailReport(const std::string& path, size_t wantRecords, uint64_t wantStopOffset)
{
  if constexpr (HasLoadReported<J>::value)
  {
    const auto r = J::loadReported(path);
    EXPECT_EQ(r.records.size(), wantRecords);
    EXPECT_EQ(r.tail, J::Tail::Torn);
    EXPECT_EQ(r.stopOffset, wantStopOffset);
  }
  else
  {
    (void)path;
    (void)wantRecords;
    (void)wantStopOffset;
  }
}

template <class J>
void expectCorruptReport(const std::string& path, size_t wantRecords, uint64_t wantStopOffset)
{
  if constexpr (HasLoadReported<J>::value)
  {
    const auto r = J::loadReported(path);
    EXPECT_EQ(r.records.size(), wantRecords);
    EXPECT_EQ(r.tail, J::Tail::Corrupt);
    EXPECT_EQ(r.stopOffset, wantStopOffset);
  }
  else
  {
    (void)path;
    (void)wantRecords;
    (void)wantStopOffset;
  }
}

template <class J>
void expectIntactReport(const std::string& path, size_t wantRecords)
{
  if constexpr (HasLoadReported<J>::value)
  {
    const auto r = J::loadReported(path);
    EXPECT_EQ(r.records.size(), wantRecords);
    EXPECT_EQ(r.tail, J::Tail::Intact);
  }
  else
  {
    (void)path;
    (void)wantRecords;
  }
}
}  // namespace

TEST(VenueJournalTornTail, ADamagedTailIsReportedAsTorn)
{
  ASSERT_TRUE(HasLoadReported<Journal>::value)
      << "needs: Journal::loadReported(path) -> LoadReport{records, Tail, stopOffset}";

  const std::string src = goodJournal("venue_report_torn_src");
  const auto bytes = readAll(src);
  const std::string path = damagedCopy("venue_report_torn", bytes, kGood - 1, kStampByte);

  expectTornTailReport<Journal>(path, kGood - 1, (kGood - 1) * kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// A record whose bytes are simply not all there -- the ordinary crash shape --
// reports the same way, so an operator reads one answer for "the tail did not
// survive" rather than two that have to be told apart.
TEST(VenueJournalTornTail, AShortFinalRecordIsReportedAsTorn)
{
  ASSERT_TRUE(HasLoadReported<Journal>::value)
      << "needs: Journal::loadReported(path) -> LoadReport{records, Tail, stopOffset}";

  const std::string src = goodJournal("venue_report_short_src");
  auto bytes = readAll(src);
  bytes.resize(bytes.size() - 6);
  const std::string path = tmpPath("venue_report_short", ".bin");
  writeAll(path, bytes);

  expectTornTailReport<Journal>(path, kGood - 1, (kGood - 1) * kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// Damage in the MIDDLE is not a crash tail: whole records follow it, so the
// file was not cut short -- it rotted. Same intact prefix, different answer,
// and the offset is what an operator needs to say how much history is behind
// the hole.
TEST(VenueJournalTornTail, DamageInTheMiddleIsReportedAsCorruptionAtItsOffset)
{
  ASSERT_TRUE(HasLoadReported<Journal>::value)
      << "needs: Journal::loadReported(path) -> LoadReport{records, Tail, stopOffset}";

  const std::string src = goodJournal("venue_report_mid_src");
  const auto bytes = readAll(src);
  constexpr size_t kBad = 4;
  // A body byte, so the framing still parses and only the crc objects.
  const std::string path =
      damagedCopy("venue_report_mid", bytes, kBad, Journal::kHeaderSize + 1);

  // The prefix ahead of the hole is intact either way; loadTimed already
  // returns it and must keep doing so.
  EXPECT_EQ(Journal::loadTimed(path).size(), kBad);
  expectCorruptReport<Journal>(path, kBad, kBad * kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// Green control for the reporting surface: an undamaged file reports Intact,
// so "torn" and "corrupt" are readings a healthy journal never produces.
TEST(VenueJournalTornTail, AnUndamagedJournalReportsAnIntactTail)
{
  ASSERT_TRUE(HasLoadReported<Journal>::value)
      << "needs: Journal::loadReported(path) -> LoadReport{records, Tail, stopOffset}";

  const std::string path = goodJournal("venue_report_intact");
  expectIntactReport<Journal>(path, kGood);
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// (4) GREEN CONTROL. The refusal that must survive the fix: a record whose crc
// PASSES and whose stamp names another version was not damaged, it was written
// by another build, and docs/venue/runtime.md:227-231 says it is refused by
// name. Reordering the stamp check behind the crc must not soften this into a
// prefix.

TEST(VenueJournalTornTail, AForeignVersionStampWithAValidCrcIsStillRefusedByName)
{
  const std::string path = goodJournal("venue_foreign_version");
  appendForeignVersionRecord(path);

  EXPECT_THROW(
      {
        try
        {
          Journal::loadTimed(path);
        }
        catch (const JournalFormatError& e)
        {
          const std::string what = e.what();
          // The two versions, by number: the one found and the one this build
          // reads. An operator has to be able to act on the message alone.
          EXPECT_NE(what.find(std::to_string(static_cast<unsigned>(kRecordVersion - 2))),
                    std::string::npos)
              << what;
          EXPECT_NE(what.find(std::to_string(static_cast<unsigned>(kRecordVersion))),
                    std::string::npos)
              << what;
          EXPECT_NE(what.find(std::to_string(kGood)), std::string::npos) << what;
          throw;
        }
      },
      JournalFormatError);

  std::remove(path.c_str());
}

// Green control, other half: an unversioned (format 0) record with a valid crc
// is named as version 0 rather than read at the wrong offsets --
// docs/venue/runtime.md:243-245.
TEST(VenueJournalTornTail, AnUnversionedRecordWithAValidCrcIsStillNamedAsVersionZero)
{
  const std::string path = goodJournal("venue_unversioned");
  {
    std::vector<uint8_t> rec;
    const auto put = [&rec](const void* p, size_t n)
    {
      const auto* b = static_cast<const uint8_t*>(p);
      rec.insert(rec.end(), b, b + n);
    };
    const int64_t ts = 8888;
    const uint8_t stamp = 0;  // bit 7 clear: written before records carried a version
    const uint8_t tag = wireTagOf(tick());
    const uint32_t len = static_cast<uint32_t>(sizeof(TimeTick));
    const TimeTick body{SYM};
    put(&ts, sizeof ts);
    put(&stamp, sizeof stamp);
    put(&tag, sizeof tag);
    put(&len, sizeof len);
    put(&body, sizeof body);
    const uint32_t crc = flox::util::Crc32::compute(rec.data(), rec.size());
    put(&crc, sizeof crc);
    std::FILE* f = std::fopen(path.c_str(), "ab");
    ASSERT_NE(f, nullptr);
    std::fwrite(rec.data(), 1, rec.size(), f);
    std::fclose(f);
  }

  EXPECT_THROW(
      {
        try
        {
          Journal::loadTimed(path);
        }
        catch (const JournalFormatError& e)
        {
          EXPECT_NE(std::string(e.what()).find("version 0"), std::string::npos) << e.what();
          throw;
        }
      },
      JournalFormatError);

  std::remove(path.c_str());
}
