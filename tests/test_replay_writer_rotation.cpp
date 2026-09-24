/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Segment rotation on the compressed writer.
//
// WriterConfig::max_segment_bytes is the only bound on how large a single
// .floxlog segment may grow. Every uncompressed write path consults it through
// maybeRotate(); the compressed path -- writeFrameToBlock, the single entry
// point for all compressed writes -- never does. A compressed recording
// therefore produces one unbounded segment no matter what the caller asked
// for, which defeats partial reads, rsync-as-you-record and per-segment
// recovery.

#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

using namespace flox;
using namespace flox::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
// Small enough that a few hundred trades must cross it several times, large
// enough to hold a whole compressed block plus its headers.
constexpr uint64_t kMaxSegmentBytes = 8 * 1024;
constexpr size_t kTradeCount = 4000;

class ReplayWriterRotationTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() / "flox_replay_writer_rotation";
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // Writes kTradeCount trades through a writer bounded at kMaxSegmentBytes.
  void writeBoundedTape(CompressionType compression)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    // A fixed first name puts rotation on the deterministic "<stem>_NNNN"
    // branch; the default wall-clock names could collide when two rotations
    // land inside one clock tick.
    cfg.output_filename = "tape.floxlog";
    cfg.max_segment_bytes = kMaxSegmentBytes;
    cfg.compression = compression;
    // Blocks of 64 events, so the compressed path reaches flushBlock (and any
    // rotation check placed around it) many times over the run.
    cfg.index_interval = 64;

    BinaryLogWriter writer(cfg);
    for (size_t i = 0; i < kTradeCount; ++i)
    {
      TradeRecord r{};
      r.exchange_ts_ns = kBaseNs + static_cast<int64_t>(i) * 1'000'000;
      r.recv_ts_ns = r.exchange_ts_ns;
      // Incompressible payload: an LZ4 block of identical records would shrink
      // far enough to hide a missing rotation behind the compression ratio.
      r.price_raw = static_cast<int64_t>(i) * 2654435761LL + 1000003LL;
      r.qty_raw = static_cast<int64_t>(i) * 40503LL + 7919LL;
      r.trade_id = i + 1;
      r.symbol_id = 1;
      writer.writeTrade(r);
    }
    writer.close();
  }

  std::vector<std::filesystem::path> segments() const
  {
    std::vector<std::filesystem::path> out;
    for (const auto& entry : std::filesystem::directory_iterator(_dir))
    {
      if (entry.is_regular_file() && entry.path().extension() == ".floxlog")
      {
        out.push_back(entry.path());
      }
    }
    return out;
  }

  uint64_t largestSegmentBytes() const
  {
    uint64_t largest = 0;
    for (const auto& p : segments())
    {
      largest = std::max<uint64_t>(largest, std::filesystem::file_size(p));
    }
    return largest;
  }

  size_t countTrades() const
  {
    ReaderConfig cfg{};
    cfg.data_dir = _dir;
    BinaryLogReader reader(cfg);
    size_t n = 0;
    reader.forEach(
        [&](const ReplayEvent& ev)
        {
          if (ev.type == EventType::Trade)
          {
            ++n;
          }
          return true;
        });
    return n;
  }

  std::filesystem::path _dir;
};

}  // namespace

// The finding. A compressed writer bounded at kMaxSegmentBytes writes far past
// that bound and still produces exactly one segment.
TEST_F(ReplayWriterRotationTest, CompressedWriterRotatesAtTheConfiguredBound)
{
  writeBoundedTape(CompressionType::LZ4);

  EXPECT_GT(segments().size(), 1u)
      << "max_segment_bytes=" << kMaxSegmentBytes
      << " was configured but the compressed writer produced a single segment";
}

// A rotation that fires but writes segments of arbitrary size is not a
// rotation. The bound may be overshot by at most one block plus the index a
// closing segment appends, never by the whole recording.
TEST_F(ReplayWriterRotationTest, CompressedSegmentsStayNearTheConfiguredBound)
{
  writeBoundedTape(CompressionType::LZ4);

  EXPECT_LE(largestSegmentBytes(), kMaxSegmentBytes * 4)
      << "the largest compressed segment is far past max_segment_bytes=" << kMaxSegmentBytes;
}

// Rotation must not cost events: every trade written is still readable across
// the rotated set. Green on the untouched tree (one segment holds everything);
// it is here to fail a "fix" that rotates by dropping the tail of a segment.
TEST_F(ReplayWriterRotationTest, CompressedRotationKeepsEveryEvent)
{
  writeBoundedTape(CompressionType::LZ4);

  EXPECT_EQ(countTrades(), kTradeCount);
}

// Control. The uncompressed path already rotates; it must keep rotating, and
// keep every event, after the compressed path is fixed. Green on the untouched
// tree.
TEST_F(ReplayWriterRotationTest, UncompressedWriterRotatesAtTheConfiguredBound)
{
  writeBoundedTape(CompressionType::None);

  EXPECT_GT(segments().size(), 1u);
  EXPECT_LE(largestSegmentBytes(), kMaxSegmentBytes * 4);
  EXPECT_EQ(countTrades(), kTradeCount);
}
