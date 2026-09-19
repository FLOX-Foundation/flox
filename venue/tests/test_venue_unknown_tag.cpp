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

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;

InboundCommand tick() { return InboundCommand{TimeTick{SYM}}; }

// Write a journal of `n` good records, then one whose tag names no command in
// this build -- what a file written by a newer venue looks like from here.
std::string journalWithForeignTag(const std::string& stem, size_t good)
{
  const std::string path = tmpPath(stem, ".bin");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    for (size_t i = 0; i < good; ++i)
    {
      j.append(tick(), static_cast<int64_t>(i) + 1);
    }
    j.flush();
  }

  // Append a record with a tag past the end of the variant. Body and crc are
  // well-formed; the ONLY thing wrong is that this build has no such command.
  std::FILE* f = std::fopen(path.c_str(), "ab");
  EXPECT_NE(f, nullptr);
  std::vector<uint8_t> rec;
  const auto put = [&rec](const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    rec.insert(rec.end(), b, b + n);
  };
  const int64_t ts = 9999;
  const uint8_t stamp = kRecordStamp;
  const uint8_t tag = static_cast<uint8_t>(std::variant_size_v<InboundCommand>);  // one past
  const uint32_t len = 8;
  const uint64_t body = 0;
  put(&ts, sizeof ts);
  put(&stamp, sizeof stamp);
  put(&tag, sizeof tag);
  put(&len, sizeof len);
  put(&body, sizeof body);
  const uint32_t crc = flox::util::Crc32::compute(rec.data(), rec.size());
  put(&crc, sizeof crc);
  std::fwrite(rec.data(), 1, rec.size(), f);
  std::fclose(f);
  return path;
}

// The whole point. A tag this build does not know used to stop the read and
// hand back the prefix as if it were the history -- recovery landing in a
// state the venue was never in, with nothing said.
TEST(UnknownTag, AForeignRecordTagIsRefusedNotSilentlyTruncated)
{
  const std::string path = journalWithForeignTag("venue_unknown_tag", 5);

  EXPECT_THROW(
      {
        try
        {
          Journal::loadTimed(path);
        }
        catch (const JournalFormatError& e)
        {
          // The two questions an operator asks first: which tag, and how far
          // in did the file stay good.
          const std::string what = e.what();
          EXPECT_NE(what.find(std::to_string(std::variant_size_v<InboundCommand>)),
                    std::string::npos)
              << what;
          EXPECT_NE(what.find("5"), std::string::npos) << what;
          throw;
        }
      },
      JournalFormatError);

  std::remove(path.c_str());
}

// A torn tail is the ordinary shape of a crash and must still be tolerated:
// the intact prefix comes back, no exception. Confusing the two would make
// every unclean shutdown look like a format problem.
TEST(UnknownTag, ATornTailIsStillTolerated)
{
  const std::string path = tmpPath("venue_torn_tail", ".bin");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    for (int i = 0; i < 5; ++i)
    {
      j.append(tick(), i + 1);
    }
    j.flush();
  }
  // Chop the last record in half.
  const auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size - 6);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); });
  EXPECT_EQ(records.size(), 4u);

  std::remove(path.c_str());
}

// A body the wrong size for a tag this build DOES know is corruption, not a
// foreign format, and stops the read without an exception -- the same as a
// torn tail, because that is what it is.
TEST(UnknownTag, AKnownTagWithTheWrongBodySizeStopsQuietly)
{
  const std::string path = tmpPath("venue_bad_len", ".bin");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    for (int i = 0; i < 3; ++i)
    {
      j.append(tick(), i + 1);
    }
    j.flush();
  }
  std::FILE* f = std::fopen(path.c_str(), "ab");
  ASSERT_NE(f, nullptr);
  std::vector<uint8_t> rec;
  const auto put = [&rec](const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    rec.insert(rec.end(), b, b + n);
  };
  const int64_t ts = 42;
  const uint8_t stamp = kRecordStamp;
  const uint8_t tag = 13;  // TimeTick: a tag this build knows
  const uint32_t len = 3;  // ... with a body that is not its size
  const uint8_t body[3] = {0, 0, 0};
  put(&ts, sizeof ts);
  put(&stamp, sizeof stamp);
  put(&tag, sizeof tag);
  put(&len, sizeof len);
  put(body, sizeof body);
  const uint32_t crc = flox::util::Crc32::compute(rec.data(), rec.size());
  put(&crc, sizeof crc);
  std::fwrite(rec.data(), 1, rec.size(), f);
  std::fclose(f);

  std::vector<std::pair<int64_t, InboundCommand>> records;
  EXPECT_NO_THROW({ records = Journal::loadTimed(path); });
  EXPECT_EQ(records.size(), 3u);

  std::remove(path.c_str());
}

}  // namespace
