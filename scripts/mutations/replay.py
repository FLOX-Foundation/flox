#!/usr/bin/env python3
"""Mutation harness for the replay fixes.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the replay changes one piece at
a time, in the source, and checks that the tests written for them go red -- and
that an unmutated tree goes green before and after.

Six acceptance binaries answer for the six findings:

    test_replay_trade_side           the aggressor byte on every reader
    test_replay_writer_rotation      the compressed writer's size bound
    test_replay_sorted_flag          the Sorted promise and what readers do with it
    test_replay_merge_stability      tie-break order on both merge surfaces
    test_replay_side_channel_frames  side-channel frames must publish no book
    test_replay_validator_overlap    overlapping segment ranges

Each mutation names the binaries that are supposed to notice and the tests
inside them; that filter runs first, and a mutation the filter does not kill is
re-run against the whole of those binaries. A mutation still alive after that
is swept against every replay, binary-log, tape, aggregator and backtest binary
in the tree before it is reported green, so a survivor is a survivor of
everything, not just of the suite written for its change.

Every run is honest about the build: the mutated file's hash is printed before
and after, the library object for the mutated file and every object of every
target asked are deleted so nothing can be served from cache, the rebuild
output has to contain "Building CXX" (or, for a mutation that touches no
compiled source, "Linking CXX") or the run is refused, and each binary runs
under a timeout. A mutation that does not compile is not a mutation and is
reported as such; so is one whose file this configuration does not build at
all.

Usage:

    python3 scripts/mutations/replay.py            # control, mutations, control
    python3 scripts/mutations/replay.py --list
    python3 scripts/mutations/replay.py --only read-books-ignore-seq

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
          -DFLOX_BUILD_TESTS=ON -DFLOX_ENABLE_BACKTEST=ON
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
BIN_DIR = BUILD / "tests"
LIB = "flox"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 1800
TEST_TIMEOUT = 300

CONNECTOR_CPP = "src/replay/replay_connector.cpp"
RUNNER_CPP = "src/backtest/backtest_runner.cpp"
PUMP_H = "include/flox/backtest/strategy_pump.h"
PREAGG_CPP = "tools/preagg_bars.cpp"
OHLCV_CPP = "src/replay/ohlcv_replay_source.cpp"
CAPI_CPP = "src/capi/flox_capi.cpp"
PYBIND_H = "python/strategy_bindings.h"
WRITER_CPP = "src/replay/binary_log_writer.cpp"
READER_CPP = "src/replay/binary_log_reader.cpp"
MERGED_CPP = "src/replay/merged_tape_reader.cpp"
SEGOPS_CPP = "src/replay/segment_ops.cpp"
VALIDATOR_CPP = "src/replay/validator.cpp"
SPEC_MD = "docs/spec/floxlog.md"

SIDE = "test_replay_trade_side"
ROTATION = "test_replay_writer_rotation"
SORTED = "test_replay_sorted_flag"
MERGE = "test_replay_merge_stability"
FRAMES = "test_replay_side_channel_frames"
OVERLAP = "test_replay_validator_overlap"

ACCEPTANCE = [SIDE, ROTATION, SORTED, MERGE, FRAMES, OVERLAP]

# Every replay, binary-log, tape, aggregator and backtest binary in the tree. A
# mutation that survives its own acceptance binaries is run against all of
# these before it is called green.
SWEEP = [
    "test_aggregator_framework",
    "test_amm_pool_replay_source",
    "test_backtest",
    "test_backtest_ctx_position",
    "test_backtest_fee_attribution",
    "test_backtest_metrics",
    "test_backtest_queue",
    "test_backtest_run_tape",
    "test_backtest_runner_hooks",
    "test_backtest_runner_venue_stack",
    "test_backtest_slippage",
    "test_bar_aggregator",
    "test_binary_log",
    "test_book_snapshot_aggregator",
    "test_mmap_bar_writer",
    "test_pool_state_binary_log",
    "test_pool_state_tape",
    "test_position_aggregator",
    "test_replay_connector",
    "test_replay_merge_stability",
    "test_replay_side_channel_frames",
    "test_replay_sorted_flag",
    "test_replay_tape_integrity",
    "test_replay_trade_side",
    "test_replay_validator_overlap",
    "test_replay_writer_rotation",
    "test_strategy_pump",
    "test_sync_bar_aggregator",
    "test_venue_golden_replay",
    "test_venue_tape",
]


@dataclass
class Edit:
    file: str
    old: str
    new: str
    occurrence: int = 1
    expected_occurrences: int = 1


@dataclass
class Mutation:
    name: str
    why: str
    file: str = ""
    old: str = ""
    new: str = ""
    # A mutation is usually one edit in one file (the three fields above). Some
    # holes only open when two places move at once, so a mutation may carry a
    # list of edits instead.
    edits: list[Edit] = field(default_factory=list)
    targets: list[str] = field(default_factory=list)
    # The tests that are supposed to notice. Run first; the whole binary runs
    # after it as the fallback.
    gtest_filter: str | None = None
    # Extra targets that only have to build -- a tool with no test of its own.
    # Their compilation is the proof the mutation is real code.
    build_also: list[str] = field(default_factory=list)
    # False for a mutation in a file no compiler reads for these binaries (a
    # doc). The rebuild then only has to relink.
    compiles: bool = True
    # Set when this configuration does not build the mutated file at all. Such
    # a mutation is reported NOTBUILT: it is not evidence either way.
    not_built: str | None = None
    occurrence: int = 1
    expected_occurrences: int = 1

    def editList(self) -> list[Edit]:
        if self.edits:
            return self.edits
        return [Edit(self.file, self.old, self.new, self.occurrence,
                     self.expected_occurrences)]

    def files(self) -> list[str]:
        seen: list[str] = []
        for e in self.editList():
            if e.file not in seen:
                seen.append(e.file)
        return seen


ROTATE_BLOCK = """    if (!flushBlock())
    {
      return false;
    }

    // A compressed segment can only end on a block boundary: the block is the
    // unit the reader decompresses, so a file cut mid-block is unreadable. The
    // bound is measured on _segment_bytes, which counts what actually reaches
    // the file (compressed), because that is the number max_segment_bytes
    // exists to cap -- partial reads, rsync-as-you-record and per-segment
    // recovery all see the file, and none of them can know the uncompressed
    // size. The overshoot is therefore at most one block plus the index the
    // closing segment appends.
    if (_segment_bytes >= _config.max_segment_bytes)
    {
      closeInternal();
      return ensureOpen();
    }"""

NOTE_ORDER = """void BinaryLogWriter::noteUncompressedEventOrder(int64_t event_ts_ns)
{
  if (_have_last_event_ts && event_ts_ns < _last_event_ts)
  {
    _segment_has_inversion = true;
  }
  _last_event_ts = event_ts_ns;
  _have_last_event_ts = true;
}"""

CLOSE_FLAG = """    // Flush any remaining events in the block buffer (for compressed mode)
    if (isCompressed() && _block_event_count > 0)
    {
      flushBlock();
    }

    // The flag is the writer's promise that exchange_ts_ns never goes
    // backwards inside the segment, and an uncompressed segment written in
    // order keeps that promise exactly as a compressed one does. Withholding
    // it there made readers buffer and re-sort every uncompressed segment, and
    // made streamForEach judge each one against a watermark carried from the
    // segment before -- dropping the head of any segment whose range started
    // behind the previous segment's.
    if (!_segment_has_inversion)
    {
      _segment_header.flags |= SegmentFlags::Sorted;
    }"""

STREAM_FROM_SORTED = """  // Sorted: straight stream with start_ts filter.
  if (iter.header().isSorted())
  {
    ReplayEvent event;
    while (iter.next(event))
    {
      if (event.timestamp_ns < start_ts_ns)
      {
        continue;
      }
"""

STREAM_STATS_SORTED = """  if (iter.header().isSorted())
  {
    ReplayEvent event;
    while (iter.next(event))
    {
      if (!passesFilter(event))
      {
        continue;
      }
      ++stats.events_read;"""

READ_BOOKS_SORT = """  std::stable_sort(pending.begin(), pending.end(),
                   [](const Pending& a, const Pending& b)
                   {
                     if (a.row.exchange_ts_ns != b.row.exchange_ts_ns)
                     {
                       return a.row.exchange_ts_ns < b.row.exchange_ts_ns;
                     }
                     if (a.row.tape_index != b.row.tape_index)
                     {
                       return a.row.tape_index < b.row.tape_index;
                     }
                     return a.row.seq < b.row.seq;
                   });"""

OVERLAP_ISSUE = """      const auto& a = result.segments[widest.index];
      const auto& b = result.segments[r.index];
      result.issues.push_back(ValidationIssue{
          .type = IssueType::SegmentRangeOverlap,
          .severity = IssueSeverity::Error,
          .message = "Segments cover overlapping time ranges: " +
                     a.path.filename().string() + " [" + std::to_string(widest.first) + ", " +
                     std::to_string(widest.last) + "] overlaps " + b.path.filename().string() +
                     " [" + std::to_string(r.first) + ", " + std::to_string(r.last) +
                     "]. The same window was recorded twice; replaying the dataset "
                     "duplicates, reorders or drops events from it.",
          .file_offset = 0,
          .event_index = 0,
          .timestamp_ns = r.first});"""

SPEC_SIDE_BYTE = """#### Side byte

`side` is the aggressor of the trade, never the resting maker, and it is encoded as `flox::Side`: **`0` = buy, `1` = sell**. Every writer of the format has always used that encoding -- the C++ recorder hook, `flox_data_writer_write_trade`, the Node and Python `DataWriter`s and all the exchange-archive importers -- so the bytes on disk are what this table says and no tape needs migrating.

What was wrong was the read side. Until this was fixed, `ReplayConnector`, `BacktestRunner::runTape`, `StrategyPump` and `preagg_bars` decoded `side == 1` as the buy and so **inverted** the aggressor of every tape they replayed, while the aggregators (`BinCountAggregator`, `VolumeBinAggregator`) and the Node and QuickJS readers decoded the same byte correctly. Anything derived from the aggressor through one of those four readers -- queue position, maker/taker classification, buy/sell-driven strategy logic, signed volume -- is inverted in results produced before the fix and has to be recomputed. The tapes themselves are unaffected, which is why the format carries no marker for this: a marker would say something about the writer, and the writer was never the side that was wrong.

"""


MUTATIONS: list[Mutation] = [
    # ---- 1. the aggressor byte, one reader at a time -----------------------
    Mutation(
        name="connector-decodes-one-as-the-buy",
        why="ReplayConnector reads the side byte back to front again, so every tape "
            "replays with its aggressor flipped",
        file=CONNECTOR_CPP,
        old="  event.trade.isBuy = (record.side == 0);",
        new="  event.trade.isBuy = (record.side == 1);",
        targets=[SIDE, FRAMES],
        gtest_filter="ReplayTradeSideTest.RecordedAggressorSurvivesTheReplayConnector",
    ),
    Mutation(
        name="backtest-runner-decodes-one-as-the-buy",
        why="the tape path of BacktestRunner flips the aggressor again, so a strategy "
            "driven off a tape sees every buy as a sell",
        file=RUNNER_CPP,
        old="    trade_ev.trade.isBuy = (event.trade.side == 0);",
        new="    trade_ev.trade.isBuy = (event.trade.side == 1);",
        targets=[SIDE],
        gtest_filter="ReplayTradeSideTest.RecordedAggressorSurvivesTheBacktestTapePath",
    ),
    Mutation(
        name="strategy-pump-decodes-one-as-the-buy",
        why="StrategyPump is the third reader of the same byte -- the monomorphic "
            "backtest path -- and it flips the aggressor back",
        file=PUMP_H,
        old="            ev.trade.isBuy = (event.trade.side == 0);",
        new="            ev.trade.isBuy = (event.trade.side == 1);",
        targets=[SIDE],
    ),
    Mutation(
        name="preagg-bars-decodes-one-as-the-buy",
        why="the pre-aggregation tool is the fourth reader; signed volume in every bar "
            "it writes comes out with the sign reversed",
        file=PREAGG_CPP,
        old="          trade.trade.isBuy = (ev.trade.side == 0);",
        new="          trade.trade.isBuy = (ev.trade.side == 1);",
        targets=[SIDE],
        not_built="FLOX_BUILD_TOOLS is OFF in the mandated configuration, and no test "
                  "binary anywhere in the tree links tools/preagg_bars.cpp -- the "
                  "fourth decoder of the side byte has no test at all",
    ),
    # ---- 2. the synthetic bar sources --------------------------------------
    Mutation(
        name="ohlcv-source-writes-one-for-a-buy",
        why="the synthetic bar source encodes a buy the way the broken readers decoded "
            "it, so a bar-driven run and a tape-driven run disagree on the aggressor of "
            "the same price move",
        file=OHLCV_CPP,
        old="""  // A bar close is synthesised as a buy, in the tape's encoding (0 = buy), so a
  // bar-driven run and a tape-driven run agree on the aggressor.
  ev.trade.side = 0;""",
        new="  ev.trade.side = 1;",
        targets=[SIDE],
        gtest_filter="ReplayTradeSideTest.OhlcvSourceEncodesABuyTheWayTheRecorderDoes",
    ),
    Mutation(
        name="capi-ohlcv-source-writes-one-for-a-buy",
        why="the C API carries its own copy of the same synthetic source",
        file=CAPI_CPP,
        old="    ev.trade.side = 0;  // buy, in the tape's encoding",
        new="    ev.trade.side = 1;",
        targets=[SIDE],
        not_built="FLOX_BUILD_CAPI is OFF in the mandated configuration, so no "
                  "compiler reads this file and no C++ test can see the change",
    ),
    Mutation(
        name="python-ohlcv-source-writes-one-for-a-buy",
        why="the pybind reader carries a third copy of the same synthetic source",
        file=PYBIND_H,
        old="    ev.trade.side = 0;  // buy, in the tape's encoding",
        new="    ev.trade.side = 1;",
        targets=[SIDE],
        not_built="FLOX_BUILD_PYTHON is OFF in the mandated configuration (no pybind11 "
                  "here), so no compiler reads this header",
    ),
    # ---- 3. the written decision about pre-fix tapes -----------------------
    Mutation(
        name="spec-side-byte-section-removed",
        why="the spec section that records what happened to tapes read before the fix "
            "is deleted; with no format marker on the tape either, nothing tells a "
            "fixed tape from a pre-fix one",
        file=SPEC_MD,
        old=SPEC_SIDE_BYTE,
        new="",
        targets=[SIDE],
        gtest_filter="ReplayTradeSideTest.FixedTapesAreTellableFromPreFixTapes:"
                     "ReplayTradeSideTest.SpecAndRecorderAgreeOnWhichByteIsTheBuy",
        compiles=False,
    ),
    # ---- 4. the compressed writer's size bound -----------------------------
    Mutation(
        name="rotation-decided-before-the-block-is-flushed",
        why="the bound is tested against the segment as it stood before this block "
            "reached the file, so the segment always closes one block later than it was "
            "asked to",
        file=WRITER_CPP,
        old=ROTATE_BLOCK,
        new="""    if (_segment_bytes >= _config.max_segment_bytes)
    {
      closeInternal();
      return ensureOpen();
    }
    if (!flushBlock())
    {
      return false;
    }""",
        targets=[ROTATION],
        gtest_filter="ReplayWriterRotationTest.*",
    ),
    Mutation(
        name="rotation-measured-on-the-uncompressed-byte-count",
        why="the bound is measured on what the events would have taken uncompressed, "
            "not on what reached the file, so max_segment_bytes stops capping the file "
            "size it exists to cap -- the number every partial read, rsync and "
            "per-segment recovery actually sees",
        file=WRITER_CPP,
        old="    if (_segment_bytes >= _config.max_segment_bytes)\n"
            "    {\n"
            "      closeInternal();\n"
            "      return ensureOpen();\n"
            "    }",
        new="    const uint64_t uncompressed_bytes =\n"
            "        static_cast<uint64_t>(_segment_header.event_count) *\n"
            "        (sizeof(FrameHeader) + sizeof(TradeRecord));\n"
            "    if (uncompressed_bytes >= _config.max_segment_bytes)\n"
            "    {\n"
            "      closeInternal();\n"
            "      return ensureOpen();\n"
            "    }",
        targets=[ROTATION],
        gtest_filter="ReplayWriterRotationTest.*",
    ),
    Mutation(
        name="rotation-off-by-one-block",
        why="a whole block of slack is added to the bound, so every compressed segment "
            "runs one block past max_segment_bytes",
        file=WRITER_CPP,
        old="    if (_segment_bytes >= _config.max_segment_bytes)\n"
            "    {\n"
            "      closeInternal();\n"
            "      return ensureOpen();\n"
            "    }",
        new="    const uint64_t one_block_slack =\n"
            "        static_cast<uint64_t>(_config.index_interval) *\n"
            "        (sizeof(FrameHeader) + sizeof(TradeRecord));\n"
            "    if (_segment_bytes >= _config.max_segment_bytes + one_block_slack)\n"
            "    {\n"
            "      closeInternal();\n"
            "      return ensureOpen();\n"
            "    }",
        targets=[ROTATION],
        gtest_filter="ReplayWriterRotationTest.*",
    ),
    Mutation(
        name="rotation-drops-the-block-that-crosses-the-bound",
        why="the segment is closed before the full block is flushed and the block "
            "buffer is thrown away with it, so every rotation costs index_interval "
            "recorded events",
        file=WRITER_CPP,
        old=ROTATE_BLOCK,
        new="""    if (_segment_bytes >= _config.max_segment_bytes)
    {
      _block_buffer.clear();
      _block_event_count = 0;
      closeInternal();
      return ensureOpen();
    }
    if (!flushBlock())
    {
      return false;
    }""",
        targets=[ROTATION],
        gtest_filter="ReplayWriterRotationTest.CompressedRotationKeepsEveryEvent",
    ),
    Mutation(
        name="rotated-segments-fall-back-to-wall-clock-names",
        why="a rotation away from a requested filename stops using the '<stem>_NNNN' "
            "suffix and takes a wall-clock name instead: two rotations inside one clock "
            "tick collide on the same path, and a digit-leading name sorts before the "
            "requested one, so a reader walking the directory by filename reads the "
            "tail of the recording first",
        file=WRITER_CPP,
        old="""  if (!_config.output_filename.empty())
  {
    const std::filesystem::path first(_config.output_filename);
    std::string ext = first.extension().string();
    if (ext.empty())
    {
      ext = ".floxlog";
    }
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%04u", _segment_number);
    return _config.output_dir / (first.stem().string() + suffix + ext);
  }

""",
        new="",
        targets=[ROTATION],
        gtest_filter="ReplayWriterRotationTest.*",
    ),
    # ---- 5. the Sorted promise ---------------------------------------------
    Mutation(
        name="sorted-set-unconditionally-on-the-uncompressed-path",
        why="an uncompressed segment is flagged Sorted whatever order it was written "
            "in, so the flag stops being a promise and readers stream a segment that "
            "really does go backwards",
        file=WRITER_CPP,
        old="    if (!_segment_has_inversion)\n"
            "    {\n"
            "      _segment_header.flags |= SegmentFlags::Sorted;\n"
            "    }",
        new="    if (!isCompressed() || !_segment_has_inversion)\n"
            "    {\n"
            "      _segment_header.flags |= SegmentFlags::Sorted;\n"
            "    }",
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    Mutation(
        name="sorted-never-set-on-the-uncompressed-path",
        why="the pre-fix rule is back: only a compressed segment may advertise the "
            "ordering it has, so an in-order uncompressed tape is re-sorted on every "
            "read and loses its head to the cross-segment watermark",
        file=WRITER_CPP,
        old="    if (!_segment_has_inversion)\n"
            "    {\n"
            "      _segment_header.flags |= SegmentFlags::Sorted;\n"
            "    }",
        new="    if (isCompressed() && !_segment_has_inversion)\n"
            "    {\n"
            "      _segment_header.flags |= SegmentFlags::Sorted;\n"
            "    }",
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    Mutation(
        name="inversion-tracker-reset-at-every-block-boundary",
        why="the ordering check forgets the last timestamp once per index_interval "
            "events, so a segment whose timestamps step backwards exactly across a "
            "block boundary is still flagged Sorted and streams unbuffered",
        file=WRITER_CPP,
        old=NOTE_ORDER,
        new="""void BinaryLogWriter::noteUncompressedEventOrder(int64_t event_ts_ns)
{
  if (_config.index_interval > 0 &&
      _segment_header.event_count % _config.index_interval == 1)
  {
    _have_last_event_ts = false;
  }
  if (_have_last_event_ts && event_ts_ns < _last_event_ts)
  {
    _segment_has_inversion = true;
  }
  _last_event_ts = event_ts_ns;
  _have_last_event_ts = true;
}""",
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    Mutation(
        name="equal-timestamps-counted-as-an-inversion",
        why="two events stamped with the same nanosecond count as going backwards, so "
            "the Sorted flag is withheld from every tape of a venue that batches its "
            "prints -- which the merge code's own comment calls the normal case",
        file=WRITER_CPP,
        old="  if (_have_last_event_ts && event_ts_ns < _last_event_ts)",
        new="  if (_have_last_event_ts && event_ts_ns <= _last_event_ts)",
        targets=[SORTED, MERGE],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    Mutation(
        name="book-frames-not-checked-for-order",
        why="only the paths that write trades feed the ordering check, so an "
            "uncompressed segment whose book updates step backwards in time is still "
            "flagged Sorted and its readers skip the reorder buffer",
        file=WRITER_CPP,
        old="    noteUncompressedEventOrder(hdr.exchange_ts_ns);\n",
        new="",
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    Mutation(
        name="sorted-decided-before-the-final-block-is-flushed",
        why="the flag is set before the last block is flushed, so the inversion that "
            "flushBlock would find between the final block and the one before it never "
            "reaches the decision -- a compressed segment that goes backwards at its "
            "tail is advertised as sorted",
        file=WRITER_CPP,
        old=CLOSE_FLAG,
        new="""    if (!_segment_has_inversion)
    {
      _segment_header.flags |= SegmentFlags::Sorted;
    }

    // Flush any remaining events in the block buffer (for compressed mode)
    if (isCompressed() && _block_event_count > 0)
    {
      flushBlock();
    }""",
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    Mutation(
        name="sorted-branch-advances-the-shared-watermark",
        why="a sorted segment feeds the cross-segment watermark again; nothing reads it "
            "on this path yet, so the only thing it does is decide how much of the "
            "*next* unsorted segment survives the reorder buffer",
        edits=[
            Edit(READER_CPP, STREAM_FROM_SORTED,
                 STREAM_FROM_SORTED +
                 "      _stream_watermark = std::max(_stream_watermark, event.timestamp_ns);\n"),
            Edit(READER_CPP, STREAM_STATS_SORTED,
                 STREAM_STATS_SORTED.replace(
                     "      ++stats.events_read;",
                     "      _stream_watermark = std::max(_stream_watermark, event.timestamp_ns);\n"
                     "      ++stats.events_read;")),
        ],
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    Mutation(
        name="sorted-branch-drops-events-behind-the-watermark",
        why="a sorted segment is judged against where the previous segment happened to "
            "end, so the head of any segment whose range starts behind it is thrown "
            "away -- recorded events lost to a reader that had a guarantee not to lose "
            "them. This is streamForEach's path (readSegmentStreaming -> "
            "streamSegmentWithStats)",
        file=READER_CPP,
        old=STREAM_STATS_SORTED,
        new=STREAM_STATS_SORTED.replace(
            "      ++stats.events_read;",
            """      if (_stream_watermark != std::numeric_limits<int64_t>::min() &&
          event.timestamp_ns < _stream_watermark - _config.reorder_window_ns)
      {
        ++stats.late_dropped;
        continue;
      }
      _stream_watermark = std::max(_stream_watermark, event.timestamp_ns);
      ++stats.events_read;"""),
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.InOrderSegmentsKeepEveryEventWhenTheirRangesOverlap",
    ),
    Mutation(
        name="sorted-from-branch-drops-events-behind-the-watermark",
        why="the same drop, one function along: streamForEachFrom walks its segments "
            "through readSegmentStreamingFrom, whose sorted branch is a second copy of "
            "the same decision and which no test in the tree reaches",
        file=READER_CPP,
        old=STREAM_FROM_SORTED,
        new=STREAM_FROM_SORTED + """      if (_stream_watermark != std::numeric_limits<int64_t>::min() &&
          event.timestamp_ns < _stream_watermark - _config.reorder_window_ns)
      {
        ++_stats.late_dropped;
        continue;
      }
      _stream_watermark = std::max(_stream_watermark, event.timestamp_ns);
""",
        targets=[SORTED],
        gtest_filter="ReplaySortedFlagTest.*",
    ),
    # ---- 6. tie-break order on the merge surfaces --------------------------
    Mutation(
        name="read-trades-sorted-unstably",
        why="std::sort is back on the trade merge, so the recorded order of a tie group "
            "wide enough to reach the quicksort partition is permuted -- a batched "
            "venue print replays in a different order every run",
        file=MERGED_CPP,
        old="  std::stable_sort(rows.begin(), rows.end(),\n"
            "                   [](const MergedTradeRow& a, const MergedTradeRow& b)",
        new="  std::sort(rows.begin(), rows.end(),\n"
            "                   [](const MergedTradeRow& a, const MergedTradeRow& b)",
        targets=[MERGE],
        gtest_filter="ReplayMergeStabilityTest.*",
    ),
    Mutation(
        name="read-books-ignore-seq",
        why="the book merge drops the venue's sequence number from the tie-break, so "
            "book updates stamped with one nanosecond come out in whatever order the "
            "recorder happened to write them",
        file=MERGED_CPP,
        old="                     if (a.row.tape_index != b.row.tape_index)\n"
            "                     {\n"
            "                       return a.row.tape_index < b.row.tape_index;\n"
            "                     }\n"
            "                     return a.row.seq < b.row.seq;",
        new="                     return a.row.tape_index < b.row.tape_index;",
        targets=[MERGE],
        gtest_filter="ReplayMergeStabilityTest.*",
    ),
    Mutation(
        name="read-books-sort-seq-descending",
        why="the sequence number is read but compared the wrong way round, so a batch "
            "the venue emitted seq-ascending replays newest first",
        file=MERGED_CPP,
        old="                     return a.row.seq < b.row.seq;",
        new="                     return a.row.seq > b.row.seq;",
        targets=[MERGE],
        gtest_filter="ReplayMergeStabilityTest.TiedBooksComeOutInSeqOrder",
    ),
    Mutation(
        name="read-books-sorted-unstably-on-the-full-key",
        why="the book merge keeps all three keys but loses stability, so records that "
            "also tie on seq -- every venue that publishes none, where seq is 0 "
            "throughout -- are permuted out of their recorded order",
        file=MERGED_CPP,
        old=READ_BOOKS_SORT,
        new=READ_BOOKS_SORT.replace("std::stable_sort(pending", "std::sort(pending"),
        targets=[MERGE],
        gtest_filter="ReplayMergeStabilityTest.*",
    ),
    Mutation(
        name="segment-merge-sorted-unstably",
        why="SegmentOps::merge shuffles a tie group again, and unlike the reader it "
            "writes the result to a file: the nondeterminism is baked into a segment "
            "every later replay reads",
        file=SEGOPS_CPP,
        old="    std::stable_sort(all_events.begin(), all_events.end(),\n"
            "                     [](const ReplayEvent& a, const ReplayEvent& b)",
        new="    std::sort(all_events.begin(), all_events.end(),\n"
            "                     [](const ReplayEvent& a, const ReplayEvent& b)",
        targets=[MERGE],
        gtest_filter="ReplayMergeStabilityTest.MergedSegmentsKeepRecordedOrderOnTiedTimestamps",
    ),
    # ---- 7. the connector's frame dispatch ---------------------------------
    Mutation(
        name="option-quote-publishes-a-phantom-book",
        why="an OptionQuote frame falls back into the book branch, which republishes "
            "the previous book event's header with both sides emptied -- a wipe of a "
            "book nobody touched",
        file=CONNECTOR_CPP,
        old="        case replay::EventType::OptionQuote:\n"
            "        case replay::EventType::PoolState:\n"
            "          break;",
        new="        case replay::EventType::OptionQuote:\n"
            "          emitBookFromRecord(event.book_header, event.bids, event.asks);\n"
            "          break;\n"
            "        case replay::EventType::PoolState:\n"
            "          break;",
        targets=[FRAMES],
        gtest_filter="ReplaySideChannelTest.*",
    ),
    Mutation(
        name="pool-state-publishes-a-phantom-book",
        why="same hole on the DEX pool-state frame",
        file=CONNECTOR_CPP,
        old="        case replay::EventType::OptionQuote:\n"
            "        case replay::EventType::PoolState:\n"
            "          break;",
        new="        case replay::EventType::OptionQuote:\n"
            "          break;\n"
            "        case replay::EventType::PoolState:\n"
            "          emitBookFromRecord(event.book_header, event.bids, event.asks);\n"
            "          break;",
        targets=[FRAMES],
        gtest_filter="ReplaySideChannelTest.*",
    ),
    # ---- 8. the dataset-level overlap check --------------------------------
    Mutation(
        name="overlap-compared-non-strictly",
        why="segments that merely touch at the seam are reported as overlapping, so the "
            "normal output of rotation -- a cut made between two frames sharing a "
            "timestamp -- is called a double recording",
        file=VALIDATOR_CPP,
        old="    if (r.first < widest.last)",
        new="    if (r.first <= widest.last)",
        targets=[OVERLAP],
        gtest_filter="ReplayValidatorOverlapTest.*",
    ),
    Mutation(
        name="overlap-compared-only-with-the-previous-segment",
        why="each segment is compared with its immediate predecessor instead of with "
            "the one reaching furthest in time, so a segment contained in an earlier, "
            "wider one is missed whenever a shorter segment sits between them",
        file=VALIDATOR_CPP,
        old="    const Range& r = ranges[k];\n"
            "    if (r.first < widest.last)",
        new="    const Range& r = ranges[k];\n"
            "    widest = ranges[k - 1];\n"
            "    if (r.first < widest.last)",
        targets=[OVERLAP],
        gtest_filter="ReplayValidatorOverlapTest.*",
    ),
    Mutation(
        name="overlap-reported-as-a-warning",
        why="the finding is downgraded to a warning, so total_errors stays at zero and "
            "the dataset validates clean -- the caller is told a tape is fit to replay "
            "when it is not",
        file=VALIDATOR_CPP,
        old="          .severity = IssueSeverity::Error,\n"
            "          .message = \"Segments cover overlapping time ranges: \"",
        new="          .severity = IssueSeverity::Warning,\n"
            "          .message = \"Segments cover overlapping time ranges: \"",
        targets=[OVERLAP],
        gtest_filter="ReplayValidatorOverlapTest.*",
    ),
    Mutation(
        name="dataset-issue-list-left-empty",
        why="the overlap is counted and the dataset is marked invalid, but no issue is "
            "put on DatasetValidationResult::issues -- the list the fix added exists so "
            "a caller can tell an overlap from a CRC failure, and it comes back empty",
        edits=[
            Edit(VALIDATOR_CPP, OVERLAP_ISSUE, "      ++result.total_errors;"),
            Edit(VALIDATOR_CPP,
                 "  result.valid = (result.corrupted_segments == 0) && !result.hasErrors();",
                 "  result.valid = (result.corrupted_segments == 0) && result.total_errors == 0;"),
        ],
        targets=[OVERLAP],
        gtest_filter="ReplayValidatorOverlapTest.*",
    ),
    Mutation(
        name="overlap-issue-names-the-wrong-segments",
        why="the issue is raised, but the two segments in its message are swapped and "
            "its timestamp points at the wrong end -- the list is populated with "
            "something that misdirects whoever reads it",
        file=VALIDATOR_CPP,
        old="      const auto& a = result.segments[widest.index];\n"
            "      const auto& b = result.segments[r.index];",
        new="      const auto& a = result.segments[r.index];\n"
            "      const auto& b = result.segments[widest.index];",
        targets=[OVERLAP],
        gtest_filter="ReplayValidatorOverlapTest.*",
    ),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_occurrence(text: str, old: str, new: str, occurrence: int, expected: int) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(
            f"mutation anchor found {count} time(s), expected {expected}:\n  {old!r}"
        )
    start = -1
    for _ in range(occurrence):
        start = text.index(old, start + 1)
    return text[:start] + new + text[start + len(old):]


def find_objects(*args: str) -> list[Path]:
    out = subprocess.run(["find", str(BUILD), "-type", "f", *args],
                         capture_output=True, text=True, check=True).stdout.split()
    return [Path(p) for p in out]


def target_objects(target: str) -> list[Path]:
    """The target's own object files, located the way `find` would."""
    return find_objects("-name", "*.o", "-path", f"*{target}.dir*")


def library_objects(source: str) -> list[Path]:
    """The library objects that have to go for a mutated source to be recompiled.

    A mutated .cpp has exactly one object in the library; a mutated header has
    none of its own, so every library object goes and the whole library is
    rebuilt rather than trusting the dependency scanner. A file that is not a
    translation unit at all (a doc) has none either way.
    """
    if source.endswith(".md"):
        return []
    if source.endswith((".h", ".inl", ".hpp")):
        return find_objects("-name", "*.o", "-path", f"*{LIB}.dir*")
    return find_objects("-name", f"{Path(source).name}.o", "-path", f"*{LIB}.dir*")


class BuildFailed(Exception):
    def __init__(self, target: str, output: str):
        super().__init__(f"rebuild of {target} failed")
        self.target = target
        self.output = output


def rebuild(target: str, requireCompile: bool = True, requireWork: bool = True) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(target, output)
    if requireCompile and "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    if requireWork and not requireCompile:
        if "Building CXX" not in output and "Linking CXX" not in output:
            raise SystemExit(
                f"rebuild of {target} neither compiled nor linked anything:\n{output[-2000:]}"
            )
    return output


def binary_path(target: str) -> Path:
    """Where the generator put this target. Most test binaries land in
    build/tests; the venue suite has its own subdirectory."""
    for candidate in (BIN_DIR / target, BUILD / "venue" / target, BUILD / target):
        if candidate.is_file():
            return candidate
    return BIN_DIR / target


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = binary_path(target)
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def ran_line(output: str) -> str:
    return next((line for line in output.splitlines()
                 if line.startswith("[==========] ") and " ran." in line), "").strip()


def report(target: str, gtest_filter: str | None, code: int, output: str) -> None:
    flt = f" --gtest_filter={gtest_filter}" if gtest_filter else ""
    print(f"  {target}{flt} -> exit {code} "
          f"({'RED, mutation killed' if code else 'green'})   {ran_line(output)}")
    if code:
        for line in [ln for ln in output.splitlines() if ln.startswith("[  FAILED  ]")][:6]:
            print(f"    {line}")


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        # Deleted first so the control binary is compiled from the source as it
        # stands right now, not served from whatever the last run left behind.
        for obj in target_objects(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target, None)
        print(f"  control {target:<36} {'green' if code == 0 else 'RED'}   {ran_line(output)}")
        ok = ok and code == 0
    return ok


def build_all(targets: list[str], sources: list[str], compiles: bool,
              build_also: list[str]) -> None:
    """Delete every object that could hide the mutation, then rebuild."""
    removed = []
    for source in sources:
        removed += library_objects(source)
    for target in list(targets) + list(build_also):
        removed += target_objects(target)
    for obj in dict.fromkeys(removed):
        obj.unlink()
    # The previous mutation's restore already deleted these, so a zero here is
    # the expected steady state; the "Building CXX" count below is the proof
    # that the mutated source was actually compiled.
    print(f"  removed {len(set(removed))} object file(s) "
          f"(0 = already deleted by the previous restore)")
    if compiles and any(not s.endswith(".md") for s in sources):
        output = rebuild(LIB)
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {LIB}: {compiled} 'Building CXX' line(s)")
    for target in list(targets) + list(build_also):
        out = rebuild(target, requireCompile=False)
        n = sum(1 for line in out.splitlines() if "Building CXX" in line)
        link = sum(1 for line in out.splitlines() if "Linking CXX" in line)
        print(f"  rebuilt {target}: {n} 'Building CXX', {link} 'Linking CXX' line(s)")


def run_mutation(m: Mutation) -> str:
    """'killed', 'alive', 'no-compile' or 'not-built'."""
    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    for f in m.files():
        print(f"  file    {f}")
        print(f"  sha256  before  {before[f]}")

    texts = dict(originals)
    for e in edits:
        texts[e.file] = replace_occurrence(texts[e.file], e.old, e.new, e.occurrence,
                                           e.expected_occurrences)
    for f, p in paths.items():
        if texts[f] == originals[f]:
            raise SystemExit(f"mutation changed nothing in {f}")
        p.write_text(texts[f])
        print(f"  sha256  mutated {sha256(p)}  {f}")

    verdict = "alive"
    try:
        if m.not_built:
            # The anchor was found and replaced, so the mutation is real; this
            # configuration simply never hands the file to a compiler, so no
            # binary here can answer for it. Saying "green" would be a lie.
            print(f"  NOT BUILT -- {m.not_built}")
            return "not-built"

        try:
            build_all(m.targets, m.files(), m.compiles, m.build_also)
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return "no-compile"

        # The tests that are supposed to notice, first.
        filterKilled = False
        if m.gtest_filter:
            for target in m.targets:
                code, output = run_test(target, m.gtest_filter)
                report(target, m.gtest_filter, code, output)
                if code:
                    filterKilled = True
                    verdict = "killed"

        # The whole of the acceptance binaries, always: it says which suite
        # actually answers for the mutation.
        if m.gtest_filter and not filterKilled:
            print("  the named tests did not notice; falling back to the whole binaries")
        red: list[str] = []
        for target in m.targets:
            code, output = run_test(target, None)
            report(target, None, code, output)
            if code:
                red.append(target)
                verdict = "killed"
        if len(red) == 1 and len(m.targets) > 1:
            print(f"  only {red[0]} answers for this one")

        # Still alive: sweep it against every replay / binary-log / tape /
        # aggregator / backtest binary in the tree before calling it green.
        if verdict == "alive":
            print("  sweeping every replay, binary-log, tape, aggregator and backtest binary")
            extra = [t for t in SWEEP if t not in m.targets]
            try:
                for target in extra:
                    # The mutation is in the library, whose object for the
                    # mutated file was already deleted and recompiled above, so
                    # these binaries only have to be linked against it again --
                    # their own sources did not change.
                    out = rebuild(target, requireCompile=False, requireWork=False)
                    del out
            except BuildFailed as e:
                print(f"  sweep build of {e.target} failed")
                print("\n".join(e.output.splitlines()[-25:]))
                return "no-compile"
            for target in extra:
                code, output = run_test(target, None)
                report(target, None, code, output)
                if code:
                    verdict = "killed"

        if verdict == "alive":
            print("  GREEN, MUTATION SURVIVED every binary asked")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for source in m.files():
            for obj in library_objects(source):
                obj.unlink()
        for target in list(m.targets) + list(m.build_also):
            for obj in target_objects(target):
                obj.unlink()

    return verdict


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<52} {', '.join(m.files())}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            f"  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "
            f"-DFLOX_BUILD_TESTS=ON -DFLOX_ENABLE_BACKTEST=ON")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = sorted({t for m in selected for t in m.targets})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD",
             "not-built": "NOTBL"}
    for m, verdict in results:
        print(f"  {label[verdict]}  {m.name:<52} {', '.join(m.files())}")
    survived = [m.name for m, v in results if v == "alive"]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    notbuilt = [m.name for m, v in results if v == "not-built"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if notbuilt:
        print(f"{len(notbuilt)} mutation(s) are in files this configuration does not "
              f"build: {', '.join(notbuilt)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
