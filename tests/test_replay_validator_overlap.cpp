/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// DatasetValidator must look at the dataset, not only at each segment.
//
// Two segments whose [first_event_ns, last_event_ns] ranges intersect mean the
// same wall-clock window was recorded twice. Readers walk segments in filename
// order and carry one watermark across them, so an overlap is replayed as
// duplicated events, as events out of order, or -- through the reorder
// buffer's late-drop -- as silently missing ones. A validator that green-lights
// such a dataset is telling the caller a tape is fit to replay when it is not.
//
// Today DatasetValidator::validate only sums the per-segment results: nothing
// compares one segment's range against another's, and the dataset below passes
// clean.

#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/ops/validator.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
constexpr int64_t kSecond = 1'000'000'000;

class ReplayValidatorOverlapTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() / "flox_replay_validator_overlap";
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // A well-formed segment covering [from_s, to_s] seconds off kBaseNs, one
  // trade per second. Nothing about it is corrupt on its own.
  void writeSegment(const std::string& name, int64_t from_s, int64_t to_s)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = name;
    cfg.index_interval = 8;

    BinaryLogWriter writer(cfg);
    uint64_t id = 1;
    for (int64_t s = from_s; s <= to_s; ++s)
    {
      TradeRecord r{};
      r.exchange_ts_ns = kBaseNs + s * kSecond;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = Price::fromDouble(100.0).raw();
      r.qty_raw = Quantity::fromDouble(1.0).raw();
      r.symbol_id = 1;
      r.trade_id = id++;
      writer.writeTrade(r);
    }
    writer.close();
  }

  std::filesystem::path _dir;
};

}  // namespace

// The finding. Two segments covering [0 s, 60 s] and [30 s, 90 s]: each is
// internally valid, the pair is not replayable.
//
// needs: a dataset-level issue the caller can name. The check here is on the
// existing surface only (`valid` and `total_errors`), because
// DatasetValidationResult carries no issue list of its own -- every
// ValidationIssue hangs off a SegmentValidationResult. The interface the code
// agent should add is `std::vector<ValidationIssue> issues;` on
// DatasetValidationResult plus an `IssueType::SegmentRangeOverlap`, reported
// at IssueSeverity::Error, so the caller can tell an overlap from a CRC
// failure. This test passes either way; the point is that the dataset must
// stop validating clean.
TEST_F(ReplayValidatorOverlapTest, OverlappingSegmentsFailValidation)
{
  writeSegment("a.floxlog", 0, 60);
  writeSegment("b.floxlog", 30, 90);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.total_segments, 2u);
  EXPECT_GE(result.total_errors, 1u)
      << "two segments covering overlapping time ranges were reported without a single error";
  EXPECT_FALSE(result.valid)
      << "a dataset whose segments overlap in time was reported as valid";
}

// Full containment is an overlap too: b sits entirely inside a.
TEST_F(ReplayValidatorOverlapTest, ContainedSegmentFailsValidation)
{
  writeSegment("a.floxlog", 0, 90);
  writeSegment("b.floxlog", 30, 60);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.total_segments, 2u);
  EXPECT_GE(result.total_errors, 1u);
  EXPECT_FALSE(result.valid);
}

// Control. Back-to-back segments that only touch at the seam are the normal
// output of rotation and must keep validating clean. Fails any "fix" that
// flags every multi-segment dataset. Green on the untouched tree.
TEST_F(ReplayValidatorOverlapTest, AdjacentSegmentsValidateClean)
{
  writeSegment("a.floxlog", 0, 60);
  writeSegment("b.floxlog", 61, 120);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.total_segments, 2u);
  EXPECT_EQ(result.total_errors, 0u);
  EXPECT_TRUE(result.valid);
}

// Control. A single segment is never an overlap. Green on the untouched tree.
TEST_F(ReplayValidatorOverlapTest, SingleSegmentValidatesClean)
{
  writeSegment("a.floxlog", 0, 60);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.total_segments, 1u);
  EXPECT_EQ(result.total_errors, 0u);
  EXPECT_TRUE(result.valid);
}
