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
// The check is dataset-level: the per-segment pass cannot see it, because each
// segment on its own is perfectly well formed. What the caller gets back is a
// typed issue on DatasetValidationResult, so an overlap can be told apart from
// a CRC failure without reading the message.

#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/ops/validator.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
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

  static const ValidationIssue* firstOverlapIssue(const DatasetValidationResult& result)
  {
    for (const auto& issue : result.issues)
    {
      if (issue.type == IssueType::SegmentRangeOverlap)
      {
        return &issue;
      }
    }
    return nullptr;
  }

  static std::vector<std::string> overlapMessages(const DatasetValidationResult& result)
  {
    std::vector<std::string> out;
    for (const auto& issue : result.issues)
    {
      if (issue.type == IssueType::SegmentRangeOverlap)
      {
        out.push_back(issue.message);
      }
    }
    return out;
  }

  static std::string rangeText(int64_t from_s, int64_t to_s)
  {
    return "[" + std::to_string(kBaseNs + from_s * kSecond) + ", " +
           std::to_string(kBaseNs + to_s * kSecond) + "]";
  }

  std::filesystem::path _dir;
};

}  // namespace

// The finding. Two segments covering [0 s, 60 s] and [30 s, 90 s]: each is
// internally valid, the pair is not replayable.
//
// This one stays on the coarse surface -- `valid` and `total_errors` -- so it
// says only that the dataset must stop validating clean.
// OverlapArrivesAsATypedDatasetIssue below reads what the caller is actually
// handed.
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

// The comparison is strict, and it has to be: rotation cuts a segment between
// two frames, so two events sharing a timestamp land either side of the seam
// and leave a.last == b.first. That is one recording split in two, replays in
// order, and must not be called a double recording. The other control leaves a
// second of daylight between the segments and so never reaches this decision.
TEST_F(ReplayValidatorOverlapTest, SegmentsTouchingAtTheSeamValidateClean)
{
  writeSegment("a.floxlog", 0, 60);
  writeSegment("b.floxlog", 60, 120);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.total_segments, 2u);
  EXPECT_EQ(result.total_errors, 0u)
      << "two segments that merely touch at a shared timestamp were called overlapping";
  EXPECT_TRUE(result.valid);
  EXPECT_TRUE(result.issues.empty());
}

// Each segment is compared against the one reaching furthest in time, not
// against the one before it. [0, 90] is the widest; [30, 40] sits inside it
// but starts after [10, 20] ended, so a predecessor comparison lets it
// through. Both containments have to be reported.
TEST_F(ReplayValidatorOverlapTest, ContainmentIsFoundPastAShorterSegment)
{
  writeSegment("a.floxlog", 0, 90);
  writeSegment("b.floxlog", 10, 20);
  writeSegment("c.floxlog", 30, 40);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.total_segments, 3u);
  EXPECT_FALSE(result.valid);

  std::vector<std::string> overlaps;
  for (const auto& issue : result.issues)
  {
    if (issue.type == IssueType::SegmentRangeOverlap)
    {
      overlaps.push_back(issue.message);
    }
  }
  ASSERT_EQ(overlaps.size(), 2u)
      << "a segment contained in an earlier, wider one was missed because a shorter "
         "segment sat between them";

  const bool names_c = std::any_of(overlaps.begin(), overlaps.end(),
                                   [](const std::string& m)
                                   { return m.find("c.floxlog") != std::string::npos; });
  EXPECT_TRUE(names_c) << "neither reported overlap mentions the contained segment";
  EXPECT_EQ(result.total_errors, 2u);
}

// The issue list is what the dataset check is for: a caller routing a bad tape
// has to tell an overlap from a CRC failure, which means the finding has to
// arrive as a typed issue and not only as a bump of total_errors. The message
// names the wider segment first and the one that runs into it second, each
// with its own range, because that is the order a human reads them in.
TEST_F(ReplayValidatorOverlapTest, OverlapArrivesAsATypedDatasetIssue)
{
  writeSegment("a.floxlog", 0, 60);
  writeSegment("b.floxlog", 30, 90);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.issues.size(), 1u)
      << "the overlap was counted but never put on the dataset's issue list";

  const ValidationIssue* issue = firstOverlapIssue(result);
  ASSERT_NE(issue, nullptr) << "the dataset issue is not typed SegmentRangeOverlap";
  EXPECT_EQ(issue->severity, IssueSeverity::Error)
      << "an unreplayable dataset was reported at a severity that leaves it valid";

  const std::string expected =
      "a.floxlog " + rangeText(0, 60) + " overlaps b.floxlog " + rangeText(30, 90);
  EXPECT_NE(issue->message.find(expected), std::string::npos)
      << "the issue names the two segments or their ranges wrongly.\n  wanted: ..."
      << expected << "...\n  got:    " << issue->message;

  // Per-segment issues stay on their own segment: nothing about an overlap
  // belongs to either file on its own.
  for (const auto& seg : result.segments)
  {
    EXPECT_TRUE(seg.issues.empty()) << "segment " << seg.path.filename()
                                    << " was blamed for a dataset-level finding";
  }
}

// The reference range has to move as the walk goes on, and "so far" is the
// whole of the rule. a. [0, 50] opens it; b. [10, 90] overlaps a and reaches
// further than a ever did; c. [60, 70] starts after a ended and runs into b
// alone. A check that finds the furthest-reaching segment once and never
// updates it reports the first pair and walks past the second -- which is
// exactly the case the furthest-reaching rule was chosen over comparing
// neighbours to catch, and it is invisible in a fixture whose first segment
// happens to be the widest.
TEST_F(ReplayValidatorOverlapTest, TheReferenceRangeWidensAsTheWalkGoesOn)
{
  writeSegment("a.floxlog", 0, 50);
  writeSegment("b.floxlog", 10, 90);
  writeSegment("c.floxlog", 60, 70);

  DatasetValidator validator;
  auto result = validator.validate(_dir);

  ASSERT_EQ(result.total_segments, 3u);
  EXPECT_FALSE(result.valid);

  const auto overlaps = overlapMessages(result);
  ASSERT_EQ(overlaps.size(), 2u)
      << "c.floxlog runs into b.floxlog and not into a.floxlog, so it is only "
         "found once the reference range has moved on from a.floxlog";

  const std::string first =
      "a.floxlog " + rangeText(0, 50) + " overlaps b.floxlog " + rangeText(10, 90);
  const std::string second =
      "b.floxlog " + rangeText(10, 90) + " overlaps c.floxlog " + rangeText(60, 70);

  EXPECT_NE(overlaps[0].find(first), std::string::npos)
      << "the first overlap names the wrong pair or the wrong ranges.\n  wanted: ..."
      << first << "...\n  got:    " << overlaps[0];
  EXPECT_NE(overlaps[1].find(second), std::string::npos)
      << "the second overlap names the wrong pair or the wrong ranges.\n  wanted: ..."
      << second << "...\n  got:    " << overlaps[1];

  EXPECT_EQ(result.total_errors, 2u);
}
