#!/usr/bin/env python3
"""Mutation harness for the journal's damaged-tail contract, the engine's
observability thread rule and the checkpoint pause gauge.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the three fixes one piece at a
time, in the source, and checks that the tests written for them go red -- and
that an unmutated tree goes green before and after.

Three acceptance binaries answer for the mutations, each with the neighbours
that already covered the same code, and a mutation is killed if ANY of them
goes red:

    test_venue_journal_torn_tail     the loader's crc-before-header contract
    test_venue_metrics_thread_rule   the snapshot rule on the scraped accessors
    test_venue_checkpoint_pause      what the pause gauge has to contain

Two build directories are used, because two of the findings are races and a
race is not visible in an optimised single run:

    build-venue-lite        cmake --preset venue-lite                (Release)
    build-venue-lite-tsan   the same cache plus -fsanitize=thread    (Debug)

A mutation with a TSan target runs that binary THREE times and counts the
mutation killed if any single run reports; on macOS a ThreadSanitizer report
aborts the process, so the exit code there is 134 rather than a gtest failure.

Every run is honest about the build: the mutated file's hash is printed before
and after, every target's object files are deleted so nothing can be served
from cache, the rebuild output has to contain "Building CXX" or the run is
refused, and each binary runs under a timeout. A mutation that does not compile
is not a mutation and is reported as such.

A mutation that survives fails the run, unless it is listed as `equivalent`
with the reason why breaking that line cannot change any observable behaviour.

Usage:

    python3 scripts/mutations/journal_metrics.py                 # everything
    python3 scripts/mutations/journal_metrics.py --list
    python3 scripts/mutations/journal_metrics.py --group journal
    python3 scripts/mutations/journal_metrics.py --only the-spin-never-yields

The build directories are expected to be configured already:

    cmake --preset venue-lite
    cmake -B build-venue-lite-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
      -DFLOX_VENUE_LITE=ON -DFLOX_BUILD_TESTS=ON -DFLOX_NATIVE=OFF
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
LITE = REPO / "build-venue-lite"
TSAN = REPO / "build-venue-lite-tsan"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 300
TSAN_RUNS = 3

JOURNAL = "venue/include/flox-venue/journal.h"
FILEIO = "include/flox/util/file_io.h"
LOCK = "venue/include/flox-venue/engine/snapshot_lock.h"
LASTLOOK = "venue/include/flox-venue/engine/last_look.h"
LASTLOOK_INL = "venue/include/flox-venue/engine/last_look.inl"
CREDIT = "venue/include/flox-venue/engine/credit.h"
PUBS = "venue/include/flox-venue/engine/publications.h"
SHARD = "venue/include/flox-venue/sequenced_shard.h"
ENGINE = "venue/include/flox-venue/matching_engine.h"

TORN = "test_venue_journal_torn_tail"
RULE = "test_venue_metrics_thread_rule"
PAUSE = "test_venue_checkpoint_pause"

# The acceptance binary first, then the binaries that already read the same
# code before this work started: a mutation the acceptance tests miss and a
# neighbour catches is a different finding from one nothing catches at all.
JOURNAL_TARGETS = [TORN, "test_venue_unknown_tag", "test_venue_reliability",
                   "test_venue_recovery", "test_venue_journal_window",
                   "test_venue_quote_ladder", "test_venue_journal_padding",
                   "test_venue_checkpoint"]
METRICS_TARGETS = [RULE, "test_venue_engine_last_look", "test_venue_engine_publications",
                   "test_venue_metrics", "test_venue_gauge_sampler"]
PAUSE_TARGETS = [PAUSE, "test_venue_checkpoint_lane", "test_venue_checkpoint",
                 "test_venue_recovery"]


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
    group: str
    why: str
    file: str = ""
    old: str = ""
    new: str = ""
    # A mutation is usually one edit in one file (the three fields above). Some
    # only exist as a pair -- a type that changes in a declaration and in its
    # definition -- so a mutation may carry a list of edits instead.
    edits: list[Edit] = field(default_factory=list)
    targets: list[str] = field(default_factory=list)
    tsan_targets: list[str] = field(default_factory=list)
    # The tests that are supposed to answer for this line. Run first; if they
    # stay green the whole binary runs anyway, so a kill by some other test in
    # the file is reported as what it is rather than counted as coverage.
    gtest_filter: str | None = None
    occurrence: int = 1
    expected_occurrences: int = 1
    equivalent: str = ""

    def editList(self) -> list[Edit]:
        if self.edits:
            return self.edits
        return [Edit(self.file, self.old, self.new, self.occurrence, self.expected_occurrences)]

    def files(self) -> list[str]:
        seen: list[str] = []
        for e in self.editList():
            if e.file not in seen:
                seen.append(e.file)
        return seen


MUTATIONS: list[Mutation] = [
    # ---- the loader: the crc decides which failure this is ----------------
    Mutation(
        name="crc-checked-after-the-header-again",
        group="journal",
        why="the stamp is tested before the crc that covers it, the way the loader used to: "
            "one flipped bit in the last record's header answers for the whole file",
        file=JOURNAL,
        old="""      const uint8_t stamp = frame[8];
      const auto refuseIfNothingRead = [&]""",
        new="""      const uint8_t stamp = frame[8];
      if (stamp != kRecordStamp)
      {
        throw JournalFormatError(versionMismatchMessage(path, report.records.size(), stamp));
      }
      const auto refuseIfNothingRead = [&]""",
        targets=JOURNAL_TARGETS,
        gtest_filter="VenueJournalTornTail.ACorruptStampByteOnTheLastRecordKeepsTheIntactPrefix",
    ),
    Mutation(
        name="a-failed-crc-is-refused-by-name",
        group="journal",
        why="damage is treated as a format statement: a record that does not verify refuses "
            "the file instead of ending the read with the intact prefix",
        file=JOURNAL,
        old="""      if (flox::util::Crc32::compute(frame.data(), frame.size()) != crc)
      {
        refuseIfNothingRead();
        report.tail = damaged;
        break;
      }""",
        new="""      if (flox::util::Crc32::compute(frame.data(), frame.size()) != crc)
      {
        throw JournalFormatError(unreadableFileMessage(path, stamp));
      }""",
        targets=JOURNAL_TARGETS,
        gtest_filter="VenueJournalTornTail.ACorrupt*",
    ),
    Mutation(
        name="corruption-is-reported-as-a-torn-tail",
        group="journal",
        why="the stop offset stays right and the tail reads Torn wherever the damage is, so "
            "rot in the middle of a segment is reported as an ordinary crash tail",
        file=JOURNAL,
        old="""        refuseIfNothingRead();
        report.tail = damaged;
        break;
      }

      if (stamp != kRecordStamp)""",
        new="""        refuseIfNothingRead();
        report.tail = Tail::Torn;
        break;
      }

      if (stamp != kRecordStamp)""",
        targets=JOURNAL_TARGETS,
        gtest_filter="VenueJournalTornTail.DamageInTheMiddleIsReportedAsCorruptionAtItsOffset",
    ),
    Mutation(
        name="stop-offset-points-past-the-damaged-record",
        group="journal",
        why="the offset names the record after the hole instead of the hole: an operator "
            "reading it skips exactly one record of history without being told",
        file=JOURNAL,
        old="""        refuseIfNothingRead();
        report.tail = damaged;
        break;
      }

      if (stamp != kRecordStamp)""",
        new="""        refuseIfNothingRead();
        report.tail = damaged;
        report.stopOffset = off + need;
        break;
      }

      if (stamp != kRecordStamp)""",
        targets=JOURNAL_TARGETS,
        gtest_filter="VenueJournalTornTail.ADamagedTailIsReportedAsTorn:"
                     "VenueJournalTornTail.DamageInTheMiddleIsReportedAsCorruptionAtItsOffset",
    ),
    Mutation(
        name="the-refuse-at-offset-zero-rule-dropped",
        group="journal",
        why="a file that reads as nothing at all comes back as an empty prefix instead of "
            "being refused by name: a foreign journal recovers as a venue with no history",
        file=JOURNAL,
        old="""      const auto refuseIfNothingRead = [&]
      {
        if (report.records.empty())
        {
          throw JournalFormatError(unreadableFileMessage(path, stamp));
        }
      };""",
        new="""      const auto refuseIfNothingRead = [&] { (void)stamp; };""",
        targets=JOURNAL_TARGETS,
    ),
    Mutation(
        name="the-truncation-exception-dropped",
        group="journal",
        why="a file that simply ends inside a record of our own shape is refused when it "
            "recovers nothing: a segment rotated and then cut stops the shard from starting",
        file=JOURNAL,
        old="""      if (need > left)
      {
        report.tail = Tail::Torn;
        break;
      }""",
        new="""      if (need > left)
      {
        refuseIfNothingRead();
        report.tail = Tail::Torn;
        break;
      }""",
        targets=JOURNAL_TARGETS,
    ),
    Mutation(
        name="load-timed-returns-nothing-unless-the-tail-is-intact",
        group="journal",
        why="the prefix is thrown away by the old entry point whenever anything is wrong, "
            "which is the whole finding restated at the caller the shard actually uses",
        file=JOURNAL,
        old="    return loadReported(path).records;",
        new="""    LoadReport r = loadReported(path);
    if (r.tail != Tail::Intact)
    {
      return {};
    }
    return r.records;""",
        targets=JOURNAL_TARGETS,
        gtest_filter="VenueJournalTornTail.ACorruptStampByteOnTheLastRecordKeepsTheIntactPrefix",
    ),
    # ---- adversarial, the loader ------------------------------------------
    Mutation(
        name="a-crc-field-of-zero-is-believed",
        group="journal",
        why="adversarial: a record whose stored crc reads zero -- a hole, a page that never "
            "made it to disk -- is taken as verified, and its header is believed after all",
        file=JOURNAL,
        old="      if (flox::util::Crc32::compute(frame.data(), frame.size()) != crc)",
        new="      if (crc != 0 && flox::util::Crc32::compute(frame.data(), frame.size()) != crc)",
        targets=JOURNAL_TARGETS,
    ),
    Mutation(
        name="the-header-length-bound-dropped",
        group="journal",
        why="adversarial: the length in an unverified header is believed up to 1 GiB instead "
            "of the largest body this build writes, so the header's number sizes the read",
        file=JOURNAL,
        old="      if (len > maxBodySize())",
        new="      if (len > (1u << 30))",
        targets=JOURNAL_TARGETS,
    ),
    Mutation(
        name="the-intact-stop-offset-is-left-at-zero",
        group="journal",
        why="adversarial: a healthy journal reports a stop offset of 0 instead of its size, so "
            "the one field that says how far the read got is wrong exactly when nothing is",
        file=JOURNAL,
        old="""      if (left == 0)
      {
        report.tail = Tail::Intact;
        break;
      }""",
        new="""      if (left == 0)
      {
        report.tail = Tail::Intact;
        report.stopOffset = 0;
        break;
      }""",
        targets=JOURNAL_TARGETS,
    ),
    Mutation(
        name="a-stub-shorter-than-a-header-reports-intact",
        group="journal",
        why="adversarial: a file whose last bytes are fewer than one header reports a clean "
            "shutdown instead of a torn tail -- the operator's crash signal, inverted",
        file=JOURNAL,
        old="""      if (left < kHeaderSize)
      {
        report.tail = Tail::Torn;  // not even a header left
        break;
      }""",
        new="""      if (left < kHeaderSize)
      {
        report.tail = Tail::Intact;  // not even a header left
        break;
      }""",
        targets=JOURNAL_TARGETS,
    ),
    # ---- the thread rule on the scraped accessors -------------------------
    Mutation(
        name="the-sampler-does-not-take-the-lock",
        group="metrics",
        why="the reader copies the last-look map without the lock: the copy is taken while "
            "the consumer is part-way through a row, so a scraped row need not balance",
        file=LASTLOOK,
        old="""    std::lock_guard<SnapshotLock> lk(statsLock_);
    return stats_;""",
        new="    return stats_;",
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
    ),
    Mutation(
        name="the-snapshot-copy-is-taken-outside-the-lock",
        group="metrics",
        why="adversarial: the lock is taken and released, and the copy is made after it -- "
            "the shape of the bug a reviewer reads past, because the lock is right there",
        file=LASTLOOK,
        old="""    std::lock_guard<SnapshotLock> lk(statsLock_);
    return stats_;""",
        new="""    {
      std::lock_guard<SnapshotLock> lk(statsLock_);
    }
    return stats_;""",
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
    ),
    Mutation(
        name="last-look-record-does-not-take-the-lock",
        group="metrics",
        why="the writing half of the rule: the consumer writes held and the outcome that "
            "balances it with no lock, so the reader's coherent copy is copied off a torn row",
        file=LASTLOOK,
        old="""    std::lock_guard<SnapshotLock> lk(statsLock_);
    LastLookStats& st = stats_[h.makerAccount];""",
        new="    LastLookStats& st = stats_[h.makerAccount];",
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
    ),
    Mutation(
        name="the-admission-setter-does-not-take-the-lock",
        group="metrics",
        why="retuning an account writes the admission table with no lock while the /metrics "
            "thread is copying it",
        file=CREDIT,
        old="""    std::lock_guard<SnapshotLock> lk(admissionLock_);
    if (p.allowedTypes == 0 && p.allowedTif == 0 && p.deny == 0)""",
        new="    if (p.allowedTypes == 0 && p.allowedTif == 0 && p.deny == 0)",
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
    ),
    Mutation(
        name="the-spinlock-exchange-becomes-a-plain-store",
        group="metrics",
        why="test-and-set without the test-and-set: both threads can read the lock free and "
            "both then claim it, so the lock excludes nobody",
        file=LOCK,
        old="""    unsigned spins = 0;
    while (held_.exchange(true, std::memory_order_acquire))
    {
      // Read-only until it looks free: a test-and-test-and-set keeps the
      // waiter off the cache line the holder is writing.
      while (held_.load(std::memory_order_relaxed))
      {""",
        new="""    unsigned spins = 0;
    while (held_.load(std::memory_order_acquire))
    {
      while (held_.load(std::memory_order_relaxed))
      {""",
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
    ),
    Mutation(
        name="the-unlock-store-is-relaxed",
        group="metrics",
        why="adversarial: the release on unlock becomes relaxed, so nothing orders the "
            "writes inside the critical section against the next thread's acquire",
        file=LOCK,
        old="  void unlock() noexcept { held_.store(false, std::memory_order_release); }",
        new="  void unlock() noexcept { held_.store(false, std::memory_order_relaxed); }",
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
    ),
    Mutation(
        name="the-spin-never-yields",
        group="metrics",
        why="the yield after 64 spins is removed, so a waiter burns its whole quantum on a "
            "descheduled holder instead of standing aside -- expected to stay green",
        file=LOCK,
        old="""        if (++spins < kSpinsBeforeYield)
        {
          continue;
        }
        spins = 0;
        // The holder may have been descheduled mid-copy. Spinning through
        // that would cost the consumer a whole quantum for a scrape.
        std::this_thread::yield();""",
        new="""        ++spins;
        continue;""",
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
        equivalent="the yield is a scheduling courtesy, not part of the lock's contract: "
                   "mutual exclusion, the copy and every number the tests read are identical "
                   "with and without it, and on a multicore host the waiter simply spins. "
                   "Nothing here can observe it; what it protects against is a preempted "
                   "holder on a loaded box, which a two-thread test cannot produce.",
    ),
    Mutation(
        name="the-resting-count-is-published-on-insert-only",
        group="metrics",
        why="the gauge is re-derived when an order rests and not when it leaves, so the "
            "published count only ever climbs",
        file=PUBS,
        old="""    orderAccount_.erase(it);
    publishRestingCount();""",
        new="    orderAccount_.erase(it);",
        targets=METRICS_TARGETS,
    ),
    Mutation(
        name="the-resting-count-is-a-plain-size_t",
        group="metrics",
        why="the atomic the /metrics thread reads the gauge off becomes an ordinary member: "
            "the read races every order that rests or leaves",
        edits=[
            Edit(file=PUBS,
                 old="  std::atomic<uint64_t> restingCount_{0};",
                 new="  uint64_t restingCount_{0};"),
            Edit(file=PUBS,
                 old="    return restingCount_.load(std::memory_order_relaxed);",
                 new="    return restingCount_;"),
            Edit(file=PUBS,
                 old="    restingCount_.store(orderAccount_.size(), std::memory_order_relaxed);",
                 new="    restingCount_ = orderAccount_.size();"),
        ],
        targets=METRICS_TARGETS,
        tsan_targets=[RULE],
    ),
    Mutation(
        name="last-look-stats-hands-out-a-reference-again",
        group="metrics",
        why="the accessor goes back to a reference into the live map: the page is built off "
            "storage the consumer keeps writing, which is a scrape of no moment at all",
        edits=[
            Edit(file=LASTLOOK,
                 old="""  std::unordered_map<uint64_t, LastLookStats> stats() const
  {
    std::lock_guard<SnapshotLock> lk(statsLock_);
    return stats_;
  }""",
                 new="""  const std::unordered_map<uint64_t, LastLookStats>& stats() const noexcept
  {
    return stats_;
  }"""),
            Edit(file=LASTLOOK_INL,
                 old="std::unordered_map<uint64_t, LastLookStats> MatchingEngine<Book>::lastLookStats() const",
                 new="const std::unordered_map<uint64_t, LastLookStats>& MatchingEngine<Book>::lastLookStats() const noexcept"),
            Edit(file=ENGINE,
                 old="  std::unordered_map<uint64_t, LastLookStats> lastLookStats() const;",
                 new="  const std::unordered_map<uint64_t, LastLookStats>& lastLookStats() const noexcept;"),
        ],
        targets=METRICS_TARGETS,
        gtest_filter="VenueMetricsThreadRule.TheAccessorsHandOutASnapshotNotLiveStorage",
    ),
    Mutation(
        name="the-admission-snapshot-misses-one-profile",
        group="metrics",
        why="adversarial: the snapshot is built row by row and drops one -- a stale scrape "
            "rather than an empty one, which is the failure that looks like a working page",
        file=CREDIT,
        old="""    std::lock_guard<SnapshotLock> lk(admissionLock_);
    return admission_;""",
        new="""    std::lock_guard<SnapshotLock> lk(admissionLock_);
    std::unordered_map<uint64_t, AdmissionProfile> out;
    out.reserve(admission_.size());
    bool dropped = false;
    for (const auto& kv : admission_)
    {
      if (!dropped && admission_.size() > 1)
      {
        dropped = true;
        continue;
      }
      out.insert(kv);
    }
    return out;""",
        targets=METRICS_TARGETS,
    ),
    # ---- the checkpoint pause gauge ---------------------------------------
    Mutation(
        name="the-pause-opens-after-the-lane-wait-again",
        group="pause",
        why="the interval opens after the wait for the previous publish and for the "
            "checkpoint lane: a shard crowded out of the lane reports a pause it did not have",
        edits=[
            Edit(file=SHARD,
                 old="""    const auto pause0 = std::chrono::steady_clock::now();
    if (mandatory)
    {
      waitCheckpointPublish();""",
                 new="""    if (mandatory)
    {
      waitCheckpointPublish();"""),
            Edit(file=SHARD,
                 old="""    auto clone = consumer_.engine().cloneForSnapshot(Book{bookProto_});""",
                 new="""    const auto pause0 = std::chrono::steady_clock::now();
    auto clone = consumer_.engine().cloneForSnapshot(Book{bookProto_});"""),
        ],
        targets=PAUSE_TARGETS,
        gtest_filter="VenueCheckpointPause.TheReportedPauseCoversAWaitForAHeldLane",
    ),
    Mutation(
        name="the-pause-closes-before-the-spawn",
        group="pause",
        why="the stamp is taken before the ckptMx_ acquire and the std::async spawn, which "
            "run on the consumer thread with matching stopped -- tens of microseconds nobody "
            "is charged for",
        file=SHARD,
        old="""    {
      // Still the consumer thread, still not matching: the mutex and the
      // thread the spawn creates are part of the stall, and a thread spawn is
      // tens of microseconds -- the same order as the clone this gauge was
      // built to report. The pause closes after the spawn returns.
      std::lock_guard<std::mutex> lk(ckptMx_);
      ckptPending_ = std::async(std::launch::async, std::move(publish)).share();
    }
    const int64_t pauseNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - pause0)
                                .count();""",
        new="""    const int64_t pauseNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - pause0)
                                .count();
    {
      std::lock_guard<std::mutex> lk(ckptMx_);
      ckptPending_ = std::async(std::launch::async, std::move(publish)).share();
    }""",
        targets=PAUSE_TARGETS,
        gtest_filter="VenueCheckpointPause.TheReportedPauseCoversTheWholeConsumerStall",
    ),
    Mutation(
        name="the-pause-is-measured-on-the-system-clock",
        group="pause",
        why="adversarial: both ends of the interval move to a clock that can be stepped, so "
            "an ntp correction inside a checkpoint invents or erases a stall",
        edits=[
            Edit(file=SHARD,
                 old="    const auto pause0 = std::chrono::steady_clock::now();",
                 new="    const auto pause0 = std::chrono::system_clock::now();"),
            Edit(file=SHARD,
                 old="""    const int64_t pauseNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - pause0)
                                .count();""",
                 new="""    const int64_t pauseNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now() - pause0)
                                .count();"""),
        ],
        targets=PAUSE_TARGETS,
    ),
    Mutation(
        name="reserve-first-block-not-called-on-truncate",
        group="pause",
        why="a fresh segment gets its first block from the first record after the pause "
            "instead of inside it: the allocation stall lands on the matching path, outside "
            "every gauge",
        file=JOURNAL,
        old="""    if (mode == OpenMode::Truncate && !flox::fileio::reserveFirstBlock(fd))
    {
      flox::fileio::closeFd(fd);
      throw std::runtime_error("Journal: cannot prepare '" + path + "' for writing");
    }
    return fd;""",
        new="    return fd;",
        targets=PAUSE_TARGETS + [TORN, "test_venue_reliability"],
    ),
    Mutation(
        name="the-reserved-byte-is-left-in-front-of-record-zero",
        group="pause",
        why="the byte written to allocate the first block is not truncated away, so every "
            "record in a fresh segment sits one byte off and the file reads as damage",
        file=FILEIO,
        old="""  if (::ftruncate(fd, 0) != 0)
  {
    return false;
  }
  return ::lseek(fd, 0, SEEK_SET) == 0;""",
        new="  return ::lseek(fd, 0, SEEK_END) >= 0;",
        targets=PAUSE_TARGETS + [TORN, "test_venue_reliability"],
    ),
    Mutation(
        name="the-pause-total-is-not-accumulated",
        group="pause",
        why="the gauge is stamped and the running total is not, so the number an operator "
            "sums a shift's stall from stays where it was",
        file=SHARD,
        old="    checkpointPauseTotalNs_.fetch_add(pauseNs, std::memory_order_relaxed);",
        new="    (void)pauseNs;",
        targets=PAUSE_TARGETS,
        gtest_filter="VenueCheckpointPause.TheGaugeStaysBoundedByTheWallClockAndAccumulates",
    ),
    Mutation(
        name="the-lane-is-not-told-about-the-pause",
        group="pause",
        why="the driver-wide view loses this shard's pause: per-shard numbers do not add up "
            "to anything an operator can act on once shards share a thread",
        file=SHARD,
        old="""    if (lane_ != nullptr)
    {
      // How long the DRIVER was stopped, by anybody on it. Per-shard pauses
      // do not add up to anything an operator can act on once shards share a
      // thread.
      lane_->notePause(pauseNs);
    }""",
        new="""    if (lane_ != nullptr)
    {
      (void)pauseNs;
    }""",
        targets=PAUSE_TARGETS,
        gtest_filter="VenueCheckpointPause.TheGaugeStaysBoundedByTheWallClockAndAccumulates",
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


def object_files(build: Path, target: str) -> list[Path]:
    """The target's own object files in one build tree, located the way `find` would."""
    if not build.is_dir():
        return []
    out = subprocess.run(
        ["find", str(build), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, target: str, output: str):
        super().__init__(f"rebuild of {target} failed")
        self.target = target
        self.output = output


def rebuild(build: Path, target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(build), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(target, output)
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} in {build.name} compiled nothing -- the result would have "
            f"been a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_test(build: Path, target: str, gtest_filter: str | None) -> tuple[int, str]:
    cmd = [str(build / "venue" / target)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def raced(output: str) -> bool:
    return "WARNING: ThreadSanitizer" in output


def control(lite_targets: list[str], tsan_targets: list[str]) -> bool:
    ok = True
    for build, targets in ((LITE, lite_targets), (TSAN, tsan_targets)):
        for target in targets:
            # Deleted first so the control binary is compiled from the source
            # as it stands right now, not served from the last run.
            for obj in object_files(build, target):
                obj.unlink()
            rebuild(build, target)
            code, output = run_test(build, target, None)
            state = "green" if code == 0 and not raced(output) else "RED"
            summary = next((line for line in output.splitlines()
                            if line.startswith("[==========] ") and " ran." in line), "")
            print(f"  control {build.name:<22} {target:<34} {state}  {summary.strip()}")
            ok = ok and state == "green"
    return ok


def report_failures(output: str) -> None:
    failed = [line for line in output.splitlines() if line.startswith("[  FAILED  ]")]
    for line in failed[:6]:
        print(f"    {line}")
    for line in output.splitlines():
        if "WARNING: ThreadSanitizer" in line:
            print(f"    {line.strip()}")
            break


def run_mutation(m: Mutation) -> tuple[str, list[str]]:
    """('killed' | 'alive' | 'no-compile', the binaries that went red)."""
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
    killers: list[str] = []
    work = [(LITE, t) for t in m.targets] + [(TSAN, t) for t in m.tsan_targets]
    try:
        for build, target in work:
            removed = object_files(build, target)
            for obj in removed:
                obj.unlink()
            print(f"  removed {len(removed)} object file(s) for {build.name}/{target}")

        try:
            for build, target in work:
                output = rebuild(build, target)
                compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
                print(f"  rebuilt {build.name}/{target}: {compiled} 'Building CXX' line(s)")
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return "no-compile", []

        for build, target in work:
            runs = TSAN_RUNS if build is TSAN else 1
            # The tests written for this line first; the whole binary after, so
            # a kill by some other test is not mistaken for coverage.
            attempts = [m.gtest_filter, None] if (m.gtest_filter and build is LITE) else [None]
            red_here = False
            for flt in attempts:
                if red_here:
                    break
                for i in range(runs):
                    code, output = run_test(build, target, flt)
                    red = code != 0 or raced(output)
                    label = f" --gtest_filter={flt}" if flt else ""
                    run_no = f" run {i + 1}/{runs}" if runs > 1 else ""
                    print(f"  {build.name}/{target}{label}{run_no} -> exit {code} "
                          f"({'RED, mutation killed' if red else 'green'})")
                    if red:
                        red_here = True
                        verdict = "killed"
                        killers.append(f"{build.name}/{target}"
                                       f"{' (whole binary)' if flt is None and m.gtest_filter else ''}")
                        report_failures(output)
                        break
        if verdict == "alive":
            print("  GREEN, MUTATION SURVIVED every binary asked")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for build, target in work:
            for obj in object_files(build, target):
                obj.unlink()

    return verdict, killers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--group", action="append", default=[],
                        help="run only these groups: journal, metrics, pause")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.group:<8} {m.name:<48} {', '.join(m.files())}")
        return 0

    for build, preset in ((LITE, "cmake --preset venue-lite"),
                          (TSAN, "cmake -B build-venue-lite-tsan ... -fsanitize=thread")):
        if not (build / "CMakeCache.txt").is_file():
            raise SystemExit(f"{build} is not configured; run\n  {preset}")

    selected = [m for m in MUTATIONS
                if (not args.only or m.name in args.only)
                and (not args.group or m.group in args.group)]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only or args.group}")
    lite_targets = sorted({t for m in selected for t in m.targets})
    tsan_targets = sorted({t for m in selected for t in m.tsan_targets})

    print("control run before the mutations")
    if not control(lite_targets, tsan_targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(lite_targets, tsan_targets)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD"}
    for m, verdict, killers in results:
        builds = "lite+tsan" if m.tsan_targets else "lite"
        print(f"  {label[verdict]}  {m.name:<48} {builds:<9} "
              f"{killers[0] if killers else ', '.join(m.targets[:1])}")
    survived = [m for m, v, _ in results if v == "alive"]
    unexpected = [m.name for m in survived if not m.equivalent]
    equivalent = [m.name for m in survived if m.equivalent]
    nobuild = [m.name for m, v, _ in results if v == "no-compile"]
    if equivalent:
        print(f"\n{len(equivalent)} equivalent mutation(s), green by design:")
        for m in survived:
            if m.equivalent:
                print(f"  {m.name}: {m.equivalent}")
    if unexpected:
        print(f"\n{len(unexpected)} mutation(s) survived: {', '.join(unexpected)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not unexpected and restored else 1


if __name__ == "__main__":
    sys.exit(main())
