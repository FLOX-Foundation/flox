#!/usr/bin/env python3
"""Mutation harness for the EventBus stop/publish, monitor, batch and health fixes.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each part of the EventBus fix
one at a time, in the source, and checks that the test written for it goes
red -- and that an unmutated tree goes green before and after.

Four pieces of the fix are covered:

    the monitor thread's period          a floor of 1 ms under stallThreshold/2
    publishBatch's bound                 a refusal, not an assert
    the consumer health book             atomics, and one report is one update
    stop() against a live publisher      the sequence line is closed and the
                                         run's claims are counted out of the ring

Every run is honest about the build: the mutated file's hash is printed before
and after, the target's object files are deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" or the run is refused,
and every binary runs under a timeout. A mutation that does not compile is not
a mutation and is reported as such.

Each check runs a gtest filter first and falls back to the whole binary when
the filter comes back green, so a mutation the named tests miss still has every
other test in that binary asked about it. A mutation that is still alive after
all of its own checks is then swept across the remaining event_bus binaries in
the RelWithDebInfo tree, so "survived" means "no event_bus test anywhere
noticed", not "the two tests I picked did not notice".

Three build directories, chosen per check:

    build           RelWithDebInfo  -- the default answer for everything
    build-release   Release         -- NDEBUG, where the old assert was nothing
    build-tsan      Debug + TSan    -- the two race reproductions; a TSan report
                                       aborts on macOS (exit 134), and a race
                                       check runs three times and is red if any
                                       run reports

Usage:

    python3 scripts/mutations/event_bus.py            # control, all mutations, control
    python3 scripts/mutations/event_bus.py --list
    python3 scripts/mutations/event_bus.py --only stop-does-not-close-the-sequence-line

The build directories are expected to be configured already:

    cmake -S . -B build         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON
    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release        -DFLOX_BUILD_TESTS=ON
    cmake -S . -B build-tsan    -DCMAKE_BUILD_TYPE=Debug -DFLOX_BUILD_TESTS=ON \\
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \\
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
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
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 120

BUILD_DIRS = {
    "build": REPO / "build",
    "build-release": REPO / "build-release",
    "build-tsan": REPO / "build-tsan",
}

BUS = "include/flox/util/eventing/event_bus.h"

HEALTH = "test_event_bus_health"
BATCH = "test_event_bus_batch"
LIFECYCLE = "test_event_bus_lifecycle"

# The header is included by most of the tree, so only the event_bus test
# targets are rebuilt -- and a surviving mutation is swept across all of them.
SWEEP_TARGETS = [
    "test_event_bus",
    BATCH,
    HEALTH,
    LIFECYCLE,
    "test_event_bus_park",
    "test_event_bus_step",
    "test_event_bus_wake_set",
]

# halt_on_error only; exitcode=66 does not apply on macOS, where a TSan report
# aborts the process (exit 134). Either way the exit status is non-zero.
TSAN_OPTIONS = "halt_on_error=1"


@dataclass
class Edit:
    old: str
    new: str
    occurrence: int = 1
    expected_occurrences: int = 1


@dataclass
class Check:
    build: str
    target: str
    test: str | None = None  # gtest_filter; None runs the whole binary
    repeats: int = 1  # a race check runs more than once


@dataclass
class Mutation:
    name: str
    why: str
    edits: list[Edit]
    checks: list[Check]
    file: str = BUS
    # A mutation that cannot change observable behaviour. Named here, with the
    # reason, so that its survival does not fail the run.
    equivalent: str | None = None
    # This run's own prediction, printed in the table next to the verdict.
    expect_survive: bool = False
    # A verdict that is not stable across runs, with the rate measured when
    # this harness was written. Printed with the table, because "killed once"
    # and "killed every time" are not the same answer.
    flaky: str | None = None

    def __post_init__(self) -> None:
        if not self.edits:
            raise ValueError(f"{self.name}: no edits")
        if not self.checks:
            raise ValueError(f"{self.name}: no checks")


MONITOR_CHECKS = [
    Check("build", HEALTH, "EventBusMonitor.*"),
    Check("build-release", HEALTH, "EventBusMonitor.*"),
]

BATCH_CHECKS = [
    Check("build", BATCH, "EventBusBatch.*"),
    Check("build-release", BATCH, "EventBusBatch.*"),
]

HEALTH_CHECKS = [
    Check("build", HEALTH, "EventBusHealth.*"),
    Check("build-tsan", HEALTH, "EventBusHealth.*", repeats=3),
]

STOP_CHECKS = [
    Check("build", LIFECYCLE, "EventBusLifecycle.*"),
    Check("build-release", LIFECYCLE, "EventBusLifecycle.*"),
    Check("build-tsan", LIFECYCLE, "EventBusLifecycle.*", repeats=3),
]


MUTATIONS: list[Mutation] = [

    # ---- the monitor thread's period ---------------------------------------

    Mutation(
        name="monitor-period-floor-removed",
        why="the 1 ms floor goes and the period is plain stallThreshold/2 again; integer "
            "milliseconds mean a 1 ms threshold halves to a zero sleep and the monitor "
            "thread holds a core running checkHealth() back to back",
        edits=[Edit(
            old="""    constexpr auto floor = std::chrono::milliseconds{1};
    const auto half = stallThreshold / 2;
    return half < floor ? floor : half;""",
            new="    return stallThreshold / 2;",
        )],
        checks=MONITOR_CHECKS,
    ),
    Mutation(
        name="monitor-period-floor-at-zero",
        why="the floor is kept but set to 0 ms, which is the same zero sleep dressed as a "
            "rule: the constant is what the floor is for, not the comparison",
        edits=[Edit(
            old="    constexpr auto floor = std::chrono::milliseconds{1};",
            new="    constexpr auto floor = std::chrono::milliseconds{0};",
        )],
        checks=MONITOR_CHECKS,
    ),
    Mutation(
        name="monitor-period-equals-the-threshold",
        why="the period is the whole threshold instead of half of it: the floor still holds, "
            "but a stall is now noticed a whole threshold late in the worst case",
        edits=[Edit(
            old="    const auto half = stallThreshold / 2;",
            new="    const auto half = stallThreshold;",
        )],
        checks=MONITOR_CHECKS,
    ),

    # ---- publishBatch's bound ----------------------------------------------

    Mutation(
        name="batch-bound-checked-after-the-reservation",
        why="the bound is asked after the sequence reservation instead of before it, and the "
            "refusal puts the reservation back. The answer publishBatch gives is unchanged; "
            "what changes is that a refused batch moves the shared sequence line and then "
            "moves it back, so a concurrent publisher can claim a sequence above the bump "
            "and have the put-back hand the same sequence out again",
        edits=[Edit(
            old="""    if (count == 0 || count > CapacityPow2 / 2)
    {
      return -1;
    }

    if (!_running.load(std::memory_order_acquire))
    {
      return -1;
    }

    PublishSeam::beforeClaim();

    const int64_t lastSeq = _next.fetch_add(static_cast<int64_t>(count),
                                            std::memory_order_acq_rel) +
                            static_cast<int64_t>(count);
    const int64_t firstSeq = lastSeq - static_cast<int64_t>(count) + 1;
    if (firstSeq < 0)
    {""",
            new="""    if (!_running.load(std::memory_order_acquire))
    {
      return -1;
    }

    PublishSeam::beforeClaim();

    const int64_t lastSeq = _next.fetch_add(static_cast<int64_t>(count),
                                            std::memory_order_acq_rel) +
                            static_cast<int64_t>(count);
    const int64_t firstSeq = lastSeq - static_cast<int64_t>(count) + 1;
    if (count == 0 || count > CapacityPow2 / 2)
    {
      _next.fetch_sub(static_cast<int64_t>(count), std::memory_order_acq_rel);
      return -1;
    }
    if (firstSeq < 0)
    {""",
        )],
        checks=BATCH_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="batch-bound-at-the-whole-ring",
        why="the bound becomes the ring instead of half of it: a batch of 33 on a ring of 64 "
            "is published, which is the documented refusal gone, and the consumers' half of "
            "the ring is written out from under them",
        edits=[Edit(
            old="    if (count == 0 || count > CapacityPow2 / 2)",
            new="    if (count == 0 || count > CapacityPow2)",
        )],
        checks=BATCH_CHECKS,
    ),
    Mutation(
        name="batch-empty-count-accepted",
        why="the count == 0 half of the bound goes, so an empty batch walks into the "
            "reservation and the gates instead of being refused up front",
        edits=[Edit(
            old="    if (count == 0 || count > CapacityPow2 / 2)",
            new="    if (count > CapacityPow2 / 2)",
        )],
        checks=BATCH_CHECKS,
        expect_survive=True,
    ),

    # ---- the consumer health book ------------------------------------------

    Mutation(
        name="health-state-loaded-relaxed",
        why="consumerHealth() and healthSnapshot() read the state relaxed instead of acquire, "
            "so a supervisor that sees STALLED is not ordered against the sequence and the "
            "instant the same sweep wrote",
        edits=[
            Edit(
                old="    return _healthBook[consumerIndex].state.load(std::memory_order_acquire);",
                new="    return _healthBook[consumerIndex].state.load(std::memory_order_relaxed);",
            ),
            Edit(
                old="      const ConsumerHealth state = _healthBook[i].state.load(std::memory_order_acquire);",
                new="      const ConsumerHealth state = _healthBook[i].state.load(std::memory_order_relaxed);",
            ),
        ],
        checks=HEALTH_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="health-version-not-bumped-before-the-write",
        why="the odd half of the seqlock goes: the version only moves after the update, so a "
            "reader that gets in between two of the writer's field stores sees the version it "
            "started with and keeps a torn report",
        edits=[Edit(
            old="""      const uint32_t v = version.load(std::memory_order_relaxed);
      version.store(v + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);""",
            new="""      const uint32_t v = version.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);""",
        )],
        checks=HEALTH_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="health-version-bumped-only-after-the-write",
        why="both version stores move below the field stores, so the odd marker is raised "
            "after the update it was supposed to cover: the reader is told a write is in "
            "progress only once it is over",
        edits=[Edit(
            old="""      const uint32_t v = version.load(std::memory_order_relaxed);
      version.store(v + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      lastSeen.store(seen, std::memory_order_relaxed);
      lastChangeTicks.store(changeTicks, std::memory_order_relaxed);
      state.store(st, std::memory_order_release);
      version.store(v + 2, std::memory_order_release);""",
            new="""      const uint32_t v = version.load(std::memory_order_relaxed);
      lastSeen.store(seen, std::memory_order_relaxed);
      lastChangeTicks.store(changeTicks, std::memory_order_relaxed);
      state.store(st, std::memory_order_release);
      version.store(v + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      version.store(v + 2, std::memory_order_release);""",
        )],
        checks=HEALTH_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="report-reads-the-state-before-the-version",
        why="the report samples the state before it takes the version it is going to validate "
            "against, so a state from before the update passes the retry check that was meant "
            "to reject exactly that",
        edits=[Edit(
            old="""      const uint32_t before = book.version.load(std::memory_order_acquire);
      if ((before & 1u) != 0u)
      {
        continue;  // a sweep is mid-update; its next store releases us
      }
      report.state = book.state.load(std::memory_order_relaxed);""",
            new="""      report.state = book.state.load(std::memory_order_relaxed);
      const uint32_t before = book.version.load(std::memory_order_acquire);
      if ((before & 1u) != 0u)
      {
        continue;  // a sweep is mid-update; its next store releases us
      }""",
        )],
        checks=HEALTH_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="checkhealth-publishes-lastseen-and-lastchange-in-two-sweeps",
        why="one sweep becomes two versioned updates: the sequence is published against the "
            "previous instant first, so a reader can catch a report whose lastSeen moved and "
            "whose lastChange did not -- progress that never happened",
        edits=[Edit(
            old="""      if (progressed || next != previous)
      {
        book.write(seen, changeTicks, next);
      }""",
            new="""      if (progressed || next != previous)
      {
        book.write(seen, book.lastChangeTicks.load(std::memory_order_relaxed), next);
        book.write(seen, changeTicks, next);
      }""",
        )],
        checks=HEALTH_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="health-version-starts-at-the-wrap",
        why="the version counter starts two short of 2^32 so the first sweep wraps it. "
            "Unsigned arithmetic wraps the same way on both sides of the seqlock, so this is "
            "here to show the wrap is harmless rather than to break anything",
        edits=[Edit(
            old="    std::atomic<uint32_t> version{0};",
            new="    std::atomic<uint32_t> version{0xFFFFFFFEu};",
        )],
        checks=HEALTH_CHECKS,
        equivalent="the seqlock only ever compares versions for equality and tests the low "
                   "bit; both survive the wrap, so no reader can tell",
        expect_survive=True,
    ),

    # ---- stop() against a live publisher -----------------------------------

    Mutation(
        name="stop-does-not-close-the-sequence-line",
        why="the exchange that puts the sequence line out of reach becomes a load, so stop() "
            "still records a boundary but leaves the line open: a publisher preempted between "
            "reading _running and its claim gets a valid sequence and walks into the ring the "
            "teardown is destroying",
        edits=[Edit(
            old="    const int64_t lastClaim = _next.exchange(kSequenceLineClosed, std::memory_order_acq_rel);",
            new="    const int64_t lastClaim = _next.load(std::memory_order_acquire);",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="closed-sequence-line-value-is-zero",
        why="the line closes at 0 instead of INT64_MIN/2, so the very next fetch_add hands "
            "back 1 -- a perfectly valid sequence -- and the close keeps nobody out",
        edits=[Edit(
            old="  static constexpr int64_t kSequenceLineClosed = std::numeric_limits<int64_t>::min() / 2;",
            new="  static constexpr int64_t kSequenceLineClosed = 0;",
        )],
        checks=STOP_CHECKS,
        flaky="killed 3 times in 10 TSan runs of EventBusLifecycle.*, never in 10 "
              "RelWithDebInfo runs; the three runs this harness makes miss it about a "
              "third of the time",
    ),
    Mutation(
        name="stop-does-not-wait-for-the-claims-to-resolve",
        why="the line is closed and the boundary recorded, but the teardown no longer waits "
            "for the publishers below it: stop() returns while one is still constructing into "
            "a slot the ring is about to destroy",
        edits=[Edit(
            old="""    // Everything below rewrites the ring, and a publisher that claimed its
    // sequence before the seal may still be constructing into a slot the
    // teardown is about to destroy.
    awaitPublishersQuiescent();

""",
            new="",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="publish-count-resolved-relaxed",
        why="the successful resolution goes back to a relaxed fetch_add, so the counter stop() "
            "acquires no longer carries the publisher's writes to the slot with it: stop() "
            "sees the number and not the memory behind it",
        edits=[Edit(
            old="      bus->_publishCount.fetch_add(static_cast<uint64_t>(count), std::memory_order_release);",
            new="      bus->_publishCount.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="abandoned-claim-not-counted-at-the-wrap-gate",
        why="a publisher that gives up at the wrap gate because the bus stopped no longer "
            "resolves the sequence it holds, so stop()'s count never comes out even",
        edits=[Edit(
            old="""        // The gate will not open again: nothing is going to consume past this
        // point. The sequence is given up on rather than written, because the
        // slot it names still holds an event a consumer has not read -- that
        // is why the gate was closed -- and stop() is what destroys it.
        abandonClaims(1);
        return false;""",
            new="        return false;",
        )],
        checks=STOP_CHECKS,
        flaky="killed only by a hang -- one timeout in the 2 TSan runs of the first pass "
              "and none in a later 10, so the verdict rests on a deadlock that usually "
              "does not happen",
    ),
    Mutation(
        name="abandoned-claim-not-counted-at-the-reclaim-fence",
        why="the other give-up path: a publisher waiting for the slowest consumer to finish "
            "with the slot leaves without resolving its sequence",
        edits=[Edit(
            old="""        if (!_running.load(std::memory_order_relaxed))
        {
          abandonClaims(1);
          return false;
        }
        reclaimBo.pause();""",
            new="""        if (!_running.load(std::memory_order_relaxed))
        {
          return false;
        }
        reclaimBo.pause();""",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="claim-guard-does-not-resolve-on-an-exception",
        why="the guard's destructor stops resolving, which leaves only the explicit "
            "resolvePublished() on the normal path: a sequence whose event constructor throws "
            "is never resolved and stop() waits for it for good",
        edits=[Edit(
            old="""    ~ClaimGuard()
    {
      if (count != 0)
      {
        bus->abandonClaims(count);
      }
    }""",
            new="    ~ClaimGuard() = default;",
        )],
        checks=STOP_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="start-does-not-reopen-the-sequence-line",
        why="start() leaves the line where stop() closed it, so the second run of a bus "
            "refuses every publish while claiming to be running",
        edits=[Edit(
            old="""    _sealed.store(false, std::memory_order_relaxed);
    _next.store(-1, std::memory_order_release);""",
            new="    _sealed.store(false, std::memory_order_relaxed);",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="start-does-not-take-the-claim-baseline",
        why="the counters are cumulative over the life of the bus, so without a baseline the "
            "second run's stop() counts the first run's resolutions as its own, finds itself "
            "already even and walks into the teardown without waiting",
        edits=[Edit(
            old="""    _claimBase.store(_publishCount.load(std::memory_order_relaxed) +
                         _abandonedClaims.load(std::memory_order_relaxed),
                     std::memory_order_release);
""",
            new="",
        )],
        checks=STOP_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="drain-on-stop-does-not-wait-for-quiescence",
        why="the consumer drains the tail without waiting for the ring to go quiet, so it "
            "stops at the first slot a still-running publisher has not stamped and calls "
            "everything behind it absent",
        edits=[Edit(
            old="""      awaitPublishersQuiescent();
      drainSlot<L>(i, l, _runLastClaim.load(std::memory_order_acquire));""",
            new="      drainSlot<L>(i, l, _runLastClaim.load(std::memory_order_acquire));",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="drain-does-not-step-over-a-given-up-sequence",
        why="the drain stops at an unstamped sequence instead of stepping over the one a "
            "publisher gave up on, stranding every accepted event another publisher had "
            "already put behind it",
        edits=[Edit(
            old="""        if (want > resolvedThrough)
        {
          break;  // the ring ends here
        }
        // A sequence claimed and then given up on when the bus stopped: it
        // will never be stamped. Stopping at it would strand every event
        // another publisher had already put behind it, and those were accepted
        // publishes. The slot holds an older event this consumer has already
        // been given -- it could not have reached `want` otherwise -- so there
        // is nothing here to deliver, only a number to step over.
        slot.seq.store(want, std::memory_order_release);
        _gating[i].v.store(required ? want : INT64_MAX, std::memory_order_release);
        seq = want;
        continue;""",
            new="""        (void)resolvedThrough;
        break;""",
        )],
        checks=STOP_CHECKS,
    ),

    # ---- adversarial -------------------------------------------------------

    Mutation(
        name="stop-wait-gives-up-after-200ms",
        why="the wait for the claims to resolve is bounded and the bound is silent: a "
            "publisher that takes longer than 200 ms inside the ring is left there and the "
            "teardown carries on",
        edits=[Edit(
            old="""    BusyBackoff resolveBo;
    while (resolvedClaims() < claims)
    {
      resolveBo.pause();
    }""",
            new="""    BusyBackoff resolveBo;
    const auto giveUpAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (resolvedClaims() < claims)
    {
      if (std::chrono::steady_clock::now() >= giveUpAt)
      {
        break;
      }
      resolveBo.pause();
    }""",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="stop-wait-gives-up-after-two-seconds",
        why="the same silent bound, set wide enough that no test's publisher reaches it. The "
            "contract is that stop() does not return until the ring is quiet, not that it "
            "waits two seconds for it",
        edits=[Edit(
            old="""    BusyBackoff resolveBo;
    while (resolvedClaims() < claims)
    {
      resolveBo.pause();
    }""",
            new="""    BusyBackoff resolveBo;
    const auto giveUpAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (resolvedClaims() < claims)
    {
      if (std::chrono::steady_clock::now() >= giveUpAt)
      {
        break;
      }
      resolveBo.pause();
    }""",
        )],
        checks=STOP_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="resolved-claims-loaded-relaxed",
        why="stop() reads the resolution counters relaxed, so the number it waits for arrives "
            "with none of the publishers' writes ordered behind it -- the count is right and "
            "the memory it stands for is not",
        edits=[Edit(
            old="""    const uint64_t published = _publishCount.load(std::memory_order_acquire);
    const uint64_t abandoned = _abandonedClaims.load(std::memory_order_acquire);
    return published + abandoned - _claimBase.load(std::memory_order_acquire);""",
            new="""    const uint64_t published = _publishCount.load(std::memory_order_relaxed);
    const uint64_t abandoned = _abandonedClaims.load(std::memory_order_relaxed);
    return published + abandoned - _claimBase.load(std::memory_order_relaxed);""",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="publish-resolves-before-it-wakes-the-waiters",
        why="the resolution moves above wakeWaiters(), which is the one ordering the fix "
            "states in so many words: a publisher counted out of the ring is still inside the "
            "bus's own condition variable, and stop() is free to tear it down around it",
        edits=[
            Edit(
                old="""    if (_wakeOnPublish)
    {
      wakeWaiters();
    }
    // Last, so that a stop() waiting the ring out also waits out the wake-up
    // this publisher is making through the bus's own condition variable.
    claim.resolvePublished();
    return lastSeq;""",
                new="""    claim.resolvePublished();
    if (_wakeOnPublish)
    {
      wakeWaiters();
    }
    return lastSeq;""",
            ),
            Edit(
                old="""    if (_wakeOnPublish)
    {
      wakeWaiters();
    }
    // Last, so that a stop() waiting the ring out also waits out the wake-up
    // this publisher is making through the bus's own condition variable.
    claim.resolvePublished();
    return {PublishResult::SUCCESS, seq};""",
                new="""    claim.resolvePublished();
    if (_wakeOnPublish)
    {
      wakeWaiters();
    }
    return {PublishResult::SUCCESS, seq};""",
            ),
        ],
        checks=STOP_CHECKS + [Check("build-tsan", BATCH, "EventBusBatch.*", repeats=3)],
        expect_survive=True,
    ),
    Mutation(
        name="a-refused-claim-reopens-the-closed-line",
        why="a publisher whose claim comes back negative treats it as counter overflow and "
            "puts the sequence line back to -1, which is exactly the recovery a stopped bus "
            "must not have: the next publisher walks into the ring being torn down",
        edits=[Edit(
            old="""    if (seq < 0)
    {
      _next.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }""",
            new="""    if (seq < 0)
    {
      _next.store(-1, std::memory_order_release);
      return false;
    }""",
        )],
        checks=STOP_CHECKS,
    ),
    Mutation(
        name="seal-flag-published-relaxed",
        why="_sealed is stored relaxed, so the consumer thread that waits on it can see the "
            "seal without seeing the boundary stored just before it and drain against a stale "
            "_runLastClaim from the previous run",
        edits=[Edit(
            old="    _sealed.store(true, std::memory_order_release);",
            new="    _sealed.store(true, std::memory_order_relaxed);",
        )],
        checks=STOP_CHECKS,
        expect_survive=True,
    ),
    Mutation(
        name="quiescence-wait-skips-the-seal",
        why="the wait no longer waits for the seal before reading the boundary. doStop() seals "
            "first so it never notices; the consumer thread draining on its way out calls the "
            "same function concurrently and reads whatever _runLastClaim happens to hold",
        edits=[Edit(
            old="""    BusyBackoff bo;
    while (!_sealed.load(std::memory_order_acquire))
    {
      bo.pause();
    }
    const int64_t lastClaim = _runLastClaim.load(std::memory_order_acquire);""",
            new="    const int64_t lastClaim = _runLastClaim.load(std::memory_order_acquire);",
        )],
        checks=STOP_CHECKS,
        expect_survive=True,
    ),
]


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------

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


def apply_edits(text: str, edits: list[Edit]) -> str:
    for e in edits:
        text = replace_occurrence(text, e.old, e.new, e.occurrence, e.expected_occurrences)
    return text


def object_files(build: Path, target: str) -> list[Path]:
    """The target's own object files, located the way `find` would."""
    out = subprocess.run(
        ["find", str(build), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, build: str, target: str, output: str):
        super().__init__(f"rebuild of {target} in {build} failed")
        self.build = build
        self.target = target
        self.output = output


def rebuild(build_key: str, target: str) -> str:
    build = BUILD_DIRS[build_key]
    result = subprocess.run(
        [CMAKE, "--build", str(build), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(build_key, target, output)
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} in {build_key} compiled nothing -- the result would have "
            f"been a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_test(build_key: str, target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = BUILD_DIRS[build_key] / "tests" / target
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    env = dict(os.environ)
    if build_key == "build-tsan":
        env["TSAN_OPTIONS"] = TSAN_OPTIONS
    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=TEST_TIMEOUT, env=env)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def failed_lines(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith("[  FAILED  ]")]


def tsan_note(code: int, output: str) -> str:
    if "WARNING: ThreadSanitizer" in output:
        return "  TSan reported a data race"
    if code == 134:
        return "  aborted (134)"
    if code == 124:
        return "  timed out"
    return ""


def prepare(pairs: list[tuple[str, str]]) -> None:
    """Delete the object files of each (build, target) and rebuild it."""
    for build_key, target in pairs:
        removed = object_files(BUILD_DIRS[build_key], target)
        for obj in removed:
            obj.unlink()
        print(f"  removed {len(removed)} object file(s) for {build_key}/{target}")
    for build_key, target in pairs:
        output = rebuild(build_key, target)
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {build_key}/{target}: {compiled} 'Building CXX' line(s)")


def control(pairs: list[tuple[str, str]]) -> bool:
    ok = True
    for build_key, target in pairs:
        for obj in object_files(BUILD_DIRS[build_key], target):
            obj.unlink()
        rebuild(build_key, target)
        code, output = run_test(build_key, target, None)
        state = "green" if code == 0 else "RED"
        summary = next((line for line in output.splitlines()
                        if line.startswith("[==========] ") and " ran." in line), "")
        print(f"  control {build_key:<14} {target:<28} {state}   {summary.strip()}"
              f"{tsan_note(code, output)}")
        if code:
            for line in failed_lines(output)[:6]:
                print(f"    {line}")
            print("    ----- control failure, output (tail) -----")
            print("\n".join("    " + x for x in output.splitlines()[-30:]))
        ok = ok and code == 0
    return ok


def run_check(c: Check) -> tuple[bool, str]:
    """Run one check. Returns (red, label of what caught it)."""
    for attempt in range(1, c.repeats + 1):
        code, output = run_test(c.build, c.target, c.test)
        flt = f" --gtest_filter={c.test}" if c.test else " (whole binary)"
        tag = f" [run {attempt}/{c.repeats}]" if c.repeats > 1 else ""
        verdict = "RED" if code else "green"
        print(f"    {c.build:<14} {c.target}{flt}{tag} -> exit {code} ({verdict})"
              f"{tsan_note(code, output)}")
        if code:
            for line in failed_lines(output)[:6]:
                print(f"      {line}")
            return True, f"{c.build}/{c.target}{flt}"

    # Whole-binary fallback: the named tests did not notice, so every other test
    # in the same binary is asked the same question.
    if c.test is not None:
        code, output = run_test(c.build, c.target, None)
        verdict = "RED" if code else "green"
        print(f"    {c.build:<14} {c.target} (whole binary, fallback) -> exit {code} "
              f"({verdict}){tsan_note(code, output)}")
        if code:
            for line in failed_lines(output)[:6]:
                print(f"      {line}")
            return True, f"{c.build}/{c.target} (whole binary)"
    return False, ""


def sweep(exclude: set[tuple[str, str]]) -> tuple[bool, str]:
    """Ask every remaining event_bus binary in the RelWithDebInfo tree."""
    print("    sweep across the remaining event_bus binaries in build/")
    for target in SWEEP_TARGETS:
        if ("build", target) in exclude:
            continue
        prepare([("build", target)])
        code, output = run_test("build", target, None)
        verdict = "RED" if code else "green"
        print(f"    build          {target} (whole binary, sweep) -> exit {code} ({verdict})")
        if code:
            for line in failed_lines(output)[:6]:
                print(f"      {line}")
            return True, f"build/{target} (sweep)"
    return False, ""


def run_mutation(m: Mutation) -> tuple[str, str]:
    """Returns (verdict, what killed it). verdict is 'killed', 'alive' or 'no-compile'."""
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    pairs: list[tuple[str, str]] = []
    for c in m.checks:
        if (c.build, c.target) not in pairs:
            pairs.append((c.build, c.target))

    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  edits   {len(m.edits)}")
    print(f"  sha256  before  {before}")

    mutated = apply_edits(original, m.edits)
    if mutated == original:
        raise SystemExit(f"{m.name}: mutation changed nothing")
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")

    verdict = "alive"
    killer = ""
    touched: set[tuple[str, str]] = set(pairs)
    try:
        try:
            prepare(pairs)
        except BuildFailed as e:
            print(f"  {e.build}/{e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return "no-compile", ""

        for c in m.checks:
            red, who = run_check(c)
            if red:
                verdict = "killed"
                killer = who
                break

        if verdict == "alive":
            try:
                red, who = sweep(touched)
            except BuildFailed as e:
                print(f"  sweep: {e.target} DID NOT COMPILE")
                red, who = False, ""
            touched |= {("build", t) for t in SWEEP_TARGETS}
            if red:
                verdict = "killed"
                killer = who

        if verdict == "alive":
            note = f" (equivalent: {m.equivalent})" if m.equivalent else ""
            print(f"  GREEN, MUTATION SURVIVED every binary asked{note}")
        else:
            print(f"  RED, mutation killed by {killer}")
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit(f"restore failed: {m.file} does not hash back to its original")
        for build_key, target in touched:
            for obj in object_files(BUILD_DIRS[build_key], target):
                obj.unlink()

    return verdict, killer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--no-control", action="store_true",
                        help="skip the control runs (for a chunked re-run)")
    parser.add_argument("--control-only", action="store_true",
                        help="run the control over every build/target pair and exit")
    parser.add_argument("--repeat-factor", type=int, default=1,
                        help="multiply every check's repeat count, to measure a flaky verdict")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            where = ", ".join(sorted({f"{c.build}/{c.target}" for c in m.checks}))
            print(f"{m.name:<52} {where}")
        return 0

    for key, build in BUILD_DIRS.items():
        if not (build / "CMakeCache.txt").is_file():
            raise SystemExit(f"{build} ({key}) is not configured; see the module docstring")

    if args.control_only:
        pairs = sorted({(c.build, c.target) for m in MUTATIONS for c in m.checks})
        print("control run")
        return 0 if control(pairs) else 1

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    if args.repeat_factor > 1:
        for m in selected:
            for c in m.checks:
                c.repeats *= args.repeat_factor

    pairs: list[tuple[str, str]] = []
    for m in selected:
        for c in m.checks:
            if (c.build, c.target) not in pairs:
                pairs.append((c.build, c.target))
    pairs.sort()

    if not args.no_control:
        print("control run before the mutations")
        if not control(pairs):
            raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m)) for m in selected]

    restored = True
    if not args.no_control:
        print("\ncontrol run after the mutations")
        restored = control(pairs)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD"}
    for m, verdict, killer in results:
        as_predicted = (verdict == "alive") == m.expect_survive
        note = "" if as_predicted or verdict == "no-compile" else "  (against prediction)"
        where = killer if killer else ", ".join(sorted({f"{c.build}/{c.target}" for c in m.checks}))
        print(f"  {label[verdict]}  {m.name:<52} {where}{note}")

    survived = [m for m, v, _ in results if v == "alive" and not m.equivalent]
    equivalent = [m for m, v, _ in results if v == "alive" and m.equivalent]
    nobuild = [m.name for m, v, _ in results if v == "no-compile"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived -- each one is a hole in the tests:")
        for m in survived:
            print(f"  {m.name}")
    if equivalent:
        print(f"\n{len(equivalent)} equivalent mutation(s), survival expected:")
        for m in equivalent:
            print(f"  {m.name}: {m.equivalent}")
    flaky = [m for m, _, _ in results if m.flaky]
    if flaky:
        print("\nverdicts that are not stable across runs:")
        for m in flaky:
            print(f"  {m.name}: {m.flaky}")
    if nobuild:
        print(f"\n{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
