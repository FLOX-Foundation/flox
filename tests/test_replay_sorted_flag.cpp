/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The Sorted segment flag on uncompressed tapes.
//
// SegmentFlags::Sorted is the writer's promise that exchange_ts_ns never goes
// backwards inside the segment. Readers act on it: forEach streams instead of
// buffering the whole segment, and streamForEach skips the bounded
// ReorderBuffer. The writer sets it only on compressed segments, so an
// uncompressed tape written strictly in order is still treated as unordered.
//
// The second cost is not just memory. streamForEach carries one watermark
// across every segment of a dataset, and the ReorderBuffer drops any event
// more than reorder_window_ns (10 s) behind it. Two in-order segments whose
// ranges overlap -- two captures of the same window, or a symbol-partitioned
// recording -- are read back with the later-named segment's head silently
// discarded, which the Sorted flag would have prevented.

#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
constexpr int64_t kSecond = 1'000'000'000;

class ReplaySortedFlagTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() / "flox_replay_sorted_flag";
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // Writes one segment named `name` holding a trade at each of `offsets_ns`,
  // in the order given.
  void writeSegment(const std::string& name, CompressionType compression,
                    const std::vector<int64_t>& offsets_ns)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = name;
    cfg.compression = compression;
    cfg.index_interval = 8;

    BinaryLogWriter writer(cfg);
    uint64_t id = 1;
    for (int64_t off : offsets_ns)
    {
      TradeRecord r{};
      r.exchange_ts_ns = kBaseNs + off;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = Price::fromDouble(100.0).raw();
      r.qty_raw = Quantity::fromDouble(1.0).raw();
      r.symbol_id = 1;
      r.trade_id = id++;
      writer.writeTrade(r);
    }
    writer.close();
  }

  SegmentHeader headerOf(const std::string& name) const
  {
    std::ifstream in(_dir / name, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot open segment " << name;
    SegmentHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    EXPECT_EQ(in.gcount(), static_cast<std::streamsize>(sizeof(header)));
    return header;
  }

  std::filesystem::path _dir;
};

std::vector<int64_t> ascending(size_t n, int64_t step_ns)
{
  std::vector<int64_t> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i)
  {
    out.push_back(static_cast<int64_t>(i) * step_ns);
  }
  return out;
}

}  // namespace

// The finding. An uncompressed segment whose events were written in
// non-decreasing timestamp order carries the same guarantee a compressed one
// does, and must advertise it.
TEST_F(ReplaySortedFlagTest, UncompressedInOrderSegmentIsFlaggedSorted)
{
  writeSegment("a.floxlog", CompressionType::None, ascending(64, kSecond));

  EXPECT_TRUE(headerOf("a.floxlog").isSorted())
      << "an uncompressed segment written in timestamp order is not flagged Sorted";
}

// The flag is a promise, not a decoration: a segment that really does go
// backwards must not carry it. Fails any "fix" that sets Sorted
// unconditionally. Green on the untouched tree.
TEST_F(ReplaySortedFlagTest, UncompressedOutOfOrderSegmentIsNotFlaggedSorted)
{
  writeSegment("a.floxlog", CompressionType::None,
               {0, 10 * kSecond, 20 * kSecond, 5 * kSecond, 30 * kSecond});

  EXPECT_FALSE(headerOf("a.floxlog").isSorted())
      << "a segment with a backwards timestamp was flagged Sorted";
}

// Control. The compressed path already flags in-order segments; it must keep
// doing so. Green on the untouched tree.
TEST_F(ReplaySortedFlagTest, CompressedInOrderSegmentIsFlaggedSorted)
{
  writeSegment("a.floxlog", CompressionType::LZ4, ascending(64, kSecond));

  EXPECT_TRUE(headerOf("a.floxlog").isSorted());
}

// The behaviour the missing flag costs. A dataset whose segments overlap in
// time is something the validator should report (it is a recording fault), but
// reporting it is not licence for the reader to lose events from it silently.
// Two uncompressed segments, each
// written strictly in order, whose ranges overlap: the reader walks them in
// filename order, so the second one opens 120 s behind the watermark the first
// one left. Unflagged, the ReorderBuffer drops its head. Flagged, each segment
// streams on its own guarantee and nothing is lost.
//
// This is the opposite of what
// test_replay_tape_integrity.cpp::WatermarkCarriesAcrossSegments asserts on
// the same shape (two internally-ordered uncompressed segments, the second
// starting behind the first's head, one event dropped). That expectation was
// written while every uncompressed segment was unsorted by construction; a
// recorded event is not the reader's to discard, and the segment that holds
// it is in order. The two cannot both stand, and this one is the contract.
TEST_F(ReplaySortedFlagTest, InOrderSegmentsKeepEveryEventWhenTheirRangesOverlap)
{
  // a: 60 s .. 120 s. b: 0 s .. 60 s.
  std::vector<int64_t> late;
  std::vector<int64_t> early;
  for (int i = 0; i <= 60; ++i)
  {
    late.push_back((60 + i) * kSecond);
    early.push_back(i * kSecond);
  }
  writeSegment("a.floxlog", CompressionType::None, late);
  writeSegment("b.floxlog", CompressionType::None, early);

  ReaderConfig cfg{};
  cfg.data_dir = _dir;
  BinaryLogReader reader(cfg);

  size_t seen = 0;
  reader.streamForEach(
      [&](const ReplayEvent& ev)
      {
        if (ev.type == EventType::Trade)
        {
          ++seen;
        }
        return true;
      });

  EXPECT_EQ(reader.stats().late_dropped, 0u)
      << "the reader dropped events from a segment that was written in order";
  EXPECT_EQ(seen, late.size() + early.size());
}
