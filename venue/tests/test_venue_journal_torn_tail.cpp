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

// A journal of `n` well-formed records, written by this build.
std::string journalOf(const std::string& stem, size_t n)
{
  const std::string path = tmpPath(stem, ".bin");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    for (size_t i = 0; i < n; ++i)
    {
      j.append(tick(), static_cast<int64_t>(i) + 1);
    }
    j.flush();
  }
  return path;
}

// A journal of kGood well-formed records, written by this build.
std::string goodJournal(const std::string& stem) { return journalOf(stem, kGood); }

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

// Zero the crc trailer of record `idx`. A hole in the file, or a page that
// never made it to disk, reads back as zeros -- and a zero is a crc value like
// any other, not a statement that the record was not checked.
std::string zeroedCrcCopy(const std::string& stem, const std::vector<uint8_t>& src, size_t idx)
{
  auto bytes = src;
  const size_t at = idx * kRecordSize + kRecordSize - sizeof(uint32_t);
  for (size_t i = 0; i < sizeof(uint32_t); ++i)
  {
    bytes[at + i] = 0;
  }
  const std::string path = tmpPath(stem, ".bin");
  writeAll(path, bytes);
  return path;
}

std::string truncatedCopy(const std::string& stem, const std::vector<uint8_t>& src, size_t keep)
{
  const std::string path = tmpPath(stem, ".bin");
  writeAll(path, std::vector<uint8_t>(src.begin(), src.begin() + static_cast<long>(keep)));
  return path;
}

// A journal of exactly one record: the file that has no prefix to fall back
// on, where "return what was read" and "refuse the file" are different answers.
std::string oneRecordJournal(const std::string& stem) { return journalOf(stem, 1); }

// The largest body any command in this build occupies on disk, derived the way
// the loader derives its own bound -- from the tag table, not from a number
// written down twice.
uint32_t largestBodySize()
{
  uint32_t m = 0;
  for (unsigned t = 0; t < 256; ++t)
  {
    const uint32_t e = Journal::expectedBodySizeForTag(static_cast<uint8_t>(t));
    m = e > m ? e : m;
  }
  return m;
}

// A header whose length field names a body far larger than any command this
// build writes, plus `filler` bytes behind it. The crc is not reachable -- the
// bytes it would cover are not there -- so the length in the header is the
// only thing the loader has, and it is the one thing it must not size a read
// from.
void appendOversizedHeader(const std::string& path, size_t filler)
{
  std::vector<uint8_t> rec;
  const auto put = [&rec](const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    rec.insert(rec.end(), b, b + n);
  };
  const int64_t ts = 4242;
  const uint8_t stamp = kRecordStamp;
  const uint8_t tag = wireTagOf(tick());
  const uint32_t len = largestBodySize() + (1u << 20);
  put(&ts, sizeof ts);
  put(&stamp, sizeof stamp);
  put(&tag, sizeof tag);
  put(&len, sizeof len);
  rec.insert(rec.end(), filler, 0xAB);

  std::FILE* f = std::fopen(path.c_str(), "ab");
  ASSERT_NE(f, nullptr);
  std::fwrite(rec.data(), 1, rec.size(), f);
  std::fclose(f);
}

// A record whose crc VERIFIES over the bytes on disk and whose length is not a
// length its tag can have. Nothing about it was damaged in transit -- the crc
// says so -- and nothing about it is a format this build could have written
// either: every command but the ladder occupies exactly one size, so a
// TimeTick claiming one byte more than a TimeTick is a record no version of
// this writer ever produced.
void appendMisSizedRecord(const std::string& path)
{
  std::vector<uint8_t> rec;
  const auto put = [&rec](const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    rec.insert(rec.end(), b, b + n);
  };
  const int64_t ts = 5555;
  const uint8_t stamp = kRecordStamp;
  const uint8_t tag = wireTagOf(tick());
  const uint32_t len = static_cast<uint32_t>(sizeof(TimeTick)) + 1;
  const TimeTick body{SYM};
  put(&ts, sizeof ts);
  put(&stamp, sizeof stamp);
  put(&tag, sizeof tag);
  put(&len, sizeof len);
  put(&body, sizeof body);
  const uint8_t oneMore = 0;
  put(&oneMore, sizeof oneMore);
  const uint32_t crc = flox::util::Crc32::compute(rec.data(), rec.size());
  put(&crc, sizeof crc);

  std::FILE* f = std::fopen(path.c_str(), "ab");
  ASSERT_NE(f, nullptr);
  std::fwrite(rec.data(), 1, rec.size(), f);
  std::fclose(f);
}

// How many bytes appendMisSizedRecord writes.
constexpr size_t kMisSizedRecordSize =
    Journal::kHeaderSize + sizeof(TimeTick) + 1 + sizeof(uint32_t);

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
// The reporting surface these tests hold the loader to.
//
// Journal::Tail : uint8_t { Intact, Torn, Corrupt };
// Journal::LoadReport {
//          std::vector<std::pair<int64_t, InboundCommand>> records;
//          Tail tail;             // Intact: the read ended on a record boundary
//                                 // Torn:   the damaged/short record is the LAST
//                                 //         thing in the file
//                                 // Corrupt: whole bytes follow the damaged
//                                 //         record, so this is not a crash tail
//          uint64_t stopOffset;   // byte offset at which the first unrecovered
//                                 // record begins (== file size when Intact)
//        };
// static LoadReport Journal::loadReported(const std::string& path);
//
// loadTimed keeps its signature and its throw-by-name behaviour for a foreign
// version and an unknown tag; loadReported is the same read with the stop
// described instead of implied. Detected by SFINAE rather than called
// outright, so that a tree without it fails the assertion below -- naming what
// is missing -- instead of failing to compile.
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
    // The offset is not "unused when nothing is wrong": it is how far the read
    // got, and on a healthy file that is the whole of it. Left at zero it says
    // the read stopped at the start of a journal it in fact read to the end.
    EXPECT_EQ(r.stopOffset, static_cast<uint64_t>(std::filesystem::file_size(path)));
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
      << "the loader must report its stop: loadReported(path) -> "
         "LoadReport{records, Tail, stopOffset}";

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
      << "the loader must report its stop: loadReported(path) -> "
         "LoadReport{records, Tail, stopOffset}";

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
      << "the loader must report its stop: loadReported(path) -> "
         "LoadReport{records, Tail, stopOffset}";

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
      << "the loader must report its stop: loadReported(path) -> "
         "LoadReport{records, Tail, stopOffset}";

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

// ---------------------------------------------------------------------------
// (5) The crc is a value, not a flag. A record whose stored crc reads zero --
// a hole in the file, a page that never made it to disk -- has not been
// checked and passed; it has not been checked at all. Believing it puts the
// loader straight back where it started, taking a damaged header at its word.

TEST(VenueJournalTornTail, AZeroedCrcTrailerOnTheLastRecordIsATornTail)
{
  const std::string src = goodJournal("venue_zero_crc_tail_src");
  const auto bytes = readAll(src);
  const std::string path = zeroedCrcCopy("venue_zero_crc_tail", bytes, kGood - 1);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); });
  EXPECT_EQ(records.size(), kGood - 1) << "a zero crc was taken as a verified record";
  expectTornTailReport<Journal>(path, kGood - 1, (kGood - 1) * kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

TEST(VenueJournalTornTail, AZeroedCrcTrailerInTheMiddleIsCorruption)
{
  const std::string src = goodJournal("venue_zero_crc_mid_src");
  const auto bytes = readAll(src);
  constexpr size_t kBad = 3;
  const std::string path = zeroedCrcCopy("venue_zero_crc_mid", bytes, kBad);

  EXPECT_EQ(Journal::loadTimed(path).size(), kBad);
  expectCorruptReport<Journal>(path, kBad, kBad * kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// (6) A tail too short to be a header at all. The earlier short-record test
// leaves 16 bytes of a 22-byte record, which is still more than one header, so
// it never reaches the branch that has nothing to read. Three bytes do.

TEST(VenueJournalTornTail, ATailShorterThanOneHeaderIsStillTorn)
{
  const std::string src = goodJournal("venue_stub_tail_src");
  const auto bytes = readAll(src);
  const std::string path =
      truncatedCopy("venue_stub_tail", bytes, (kGood - 1) * kRecordSize + 3);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); });
  EXPECT_EQ(records.size(), kGood - 1);
  // Three bytes left is not a clean shutdown: an operator reading Intact here
  // would take a crashed venue for a stopped one.
  expectTornTailReport<Journal>(path, kGood - 1, (kGood - 1) * kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// (7) The other half of "the largest intact prefix": a prefix has to be a
// prefix OF SOMETHING. A file whose very first record is there and does not
// verify reads as nothing at all, and handing back an empty result is not a
// prefix -- it is the whole history gone with nothing said, which is what
// recovery would then serve.

TEST(VenueJournalTornTail, AFileWhoseFirstRecordDoesNotVerifyIsRefusedByName)
{
  const std::string src = oneRecordJournal("venue_first_bad_src");
  const auto bytes = readAll(src);
  ASSERT_EQ(bytes.size(), kRecordSize);
  const std::string path = damagedCopy("venue_first_bad", bytes, 0, Journal::kHeaderSize + 1);

  EXPECT_THROW(
      {
        try
        {
          Journal::loadTimed(path);
        }
        catch (const JournalFormatError& e)
        {
          const std::string what = e.what();
          EXPECT_NE(what.find(path), std::string::npos) << what;
          EXPECT_NE(what.find("record 0"), std::string::npos) << what;
          throw;
        }
      },
      JournalFormatError);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// The exception to that rule, and it has to stay an exception: a file that
// simply ENDS inside a record of our own shape was not laid out by other
// rules, it was an append caught by a power cut. A segment rotated and then
// cut inside its first record must still let the shard start, on an empty
// prefix.
TEST(VenueJournalTornTail, ASegmentCutInsideItsFirstRecordLoadsAsAnEmptyTornPrefix)
{
  const std::string src = oneRecordJournal("venue_first_cut_src");
  const auto bytes = readAll(src);

  const std::string inBody =
      truncatedCopy("venue_first_cut_body", bytes, Journal::kHeaderSize + 2);
  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(inBody); })
      << "a segment cut mid-append is a crash, not an unreadable file";
  EXPECT_EQ(records.size(), 0u);
  expectTornTailReport<Journal>(inBody, 0u, 0u);

  // Cut before even a whole header: the same answer, for the same reason.
  const std::string inHeader = truncatedCopy("venue_first_cut_header", bytes, 3);
  EXPECT_NO_THROW({ records = Journal::loadTimed(inHeader); });
  EXPECT_EQ(records.size(), 0u);
  expectTornTailReport<Journal>(inHeader, 0u, 0u);

  // And a shard comes up on it rather than refusing to start.
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  auto shard = std::make_unique<SequencedShard<>>(c, inBody, MatchingBook{}, Journal::Sync::Off);
  shard->setOwnThreads(false);
  EXPECT_NO_THROW({ shard->start(); });
  EXPECT_TRUE(shard->ready());
  EXPECT_EQ(shard->recoveredCommands(), 0u);
  shard->stop();
  shard.reset();

  std::remove(src.c_str());
  std::remove(inBody.c_str());
  std::remove(inHeader.c_str());
}

// ---------------------------------------------------------------------------
// (8) The length field is the one number in the header that has to be doubted
// before the crc can be reached, because the crc lives behind whatever it
// says. Bounded by the largest body this build writes: a truncated record of
// ours still names one of our lengths, so a length past that bound describes
// no record this build could have produced.

TEST(VenueJournalTornTail, AHeaderLengthPastTheLargestBodyIsNotBelieved)
{
  // As record 0: nothing was read, so the file is refused by name rather than
  // coming back as an empty, plausible-looking prefix.
  const std::string alone = tmpPath("venue_len_bound_alone", ".bin");
  std::remove(alone.c_str());
  writeAll(alone, {});
  appendOversizedHeader(alone, /*filler=*/1);
  EXPECT_THROW(Journal::loadTimed(alone), JournalFormatError)
      << "a length no record of ours could carry was sized a read from";

  // Behind a good prefix, with bytes after it: damage in the middle of a file,
  // not a tail cut short, and the offset is where the hole starts.
  const std::string after = goodJournal("venue_len_bound_after");
  appendOversizedHeader(after, /*filler=*/1);
  EXPECT_EQ(Journal::loadTimed(after).size(), kGood);
  expectCorruptReport<Journal>(after, kGood, kGood * kRecordSize);

  std::remove(alone.c_str());
  std::remove(after.c_str());
}

// ---------------------------------------------------------------------------
// (9) One record is a prefix too.
//
// "A stop that recovers nothing is a file this build cannot read" is the rule;
// a stop that recovers ONE record is not that. The file said something this
// build understood before the damage began, so the damage is damage and the
// record ahead of it is history -- the same answer a nine-record prefix gets,
// and for the same reason. A loader that refused here would throw away a
// shard's first command because its second one rotted.

TEST(VenueJournalTornTail, AFileWhoseSecondRecordIsDamagedKeepsTheFirst)
{
  const std::string src = journalOf("venue_one_then_bad_src", 2);
  const auto bytes = readAll(src);
  ASSERT_EQ(bytes.size(), kRecordSize * 2);
  const std::string path = damagedCopy("venue_one_then_bad", bytes, 1, Journal::kHeaderSize + 1);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); })
      << "one recovered record is a prefix, not an unreadable file";
  EXPECT_EQ(records.size(), 1u);
  expectTornTailReport<Journal>(path, 1u, kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// The same file with a good record behind the damage: still one record back,
// and the damage named as the rot it is rather than as a crash tail.
TEST(VenueJournalTornTail, AFileWhoseSecondOfThreeRecordsIsDamagedKeepsTheFirst)
{
  const std::string src = journalOf("venue_one_then_bad3_src", 3);
  const auto bytes = readAll(src);
  const std::string path = damagedCopy("venue_one_then_bad3", bytes, 1, Journal::kHeaderSize + 1);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); });
  EXPECT_EQ(records.size(), 1u);
  expectCorruptReport<Journal>(path, 1u, kRecordSize);

  std::remove(src.c_str());
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// (10) A length its own tag cannot have, on a record whose crc verifies.
//
// The crc rules out damage in transit: these bytes are exactly the bytes
// somebody wrote. What they are not is a record this build could have written
// -- every command but the ladder occupies the one size its type has, so a
// length that disagrees with the tag is a frame laid out by other rules. It
// must never be decoded: the decoder would read the fields of one command out
// of a body that is not that command's, and hand the engine a command nobody
// sent.

TEST(VenueJournalTornTail, AMisSizedRecordAtOffsetZeroIsRefusedByName)
{
  const std::string path = tmpPath("venue_missized_alone", ".bin");
  std::remove(path.c_str());
  writeAll(path, {});
  appendMisSizedRecord(path);

  EXPECT_THROW(
      {
        try
        {
          Journal::loadTimed(path);
        }
        catch (const JournalFormatError& e)
        {
          const std::string what = e.what();
          EXPECT_NE(what.find(path), std::string::npos) << what;
          EXPECT_NE(what.find("record 0"), std::string::npos) << what;
          throw;
        }
      },
      JournalFormatError)
      << "a body the wrong size for its tag was decoded as though it were the right one";

  std::remove(path.c_str());
}

TEST(VenueJournalTornTail, AMisSizedRecordBehindAGoodPrefixIsDamageAtItsOffset)
{
  // With a good record behind it: whole bytes follow, so the file rotted
  // rather than ended.
  const std::string mid = goodJournal("venue_missized_mid");
  appendMisSizedRecord(mid);
  {
    Journal j(mid, Journal::Sync::Off, Journal::OpenMode::Append);
    j.append(tick(), 9999);
    j.flush();
  }
  EXPECT_EQ(Journal::loadTimed(mid).size(), kGood)
      << "a body the wrong size for its tag was decoded as though it were the right one";
  expectCorruptReport<Journal>(mid, kGood, kGood * kRecordSize);

  // As the last record in the file: the same refusal to decode, reported as a
  // tail rather than as rot.
  const std::string tail = goodJournal("venue_missized_tail");
  appendMisSizedRecord(tail);
  ASSERT_EQ(std::filesystem::file_size(tail), kGood * kRecordSize + kMisSizedRecordSize);
  EXPECT_EQ(Journal::loadTimed(tail).size(), kGood);
  expectTornTailReport<Journal>(tail, kGood, kGood * kRecordSize);

  std::remove(mid.c_str());
  std::remove(tail.c_str());
}
