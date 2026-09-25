#!/usr/bin/env python3
"""Mutation harness for the bar-aggregator fix (Renko brick geometry, the gap
bound, and dropped late trades in a time bar).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the fix one piece at a time,
in the source, and checks that the tests written for it go red -- and that an
unmutated tree goes green before and after.

The fix touches four independent copies of the same close path
(BarAggregator::onTrade, MultiTimeframeAggregator::processPolicy, doAggregateC
in the C ABI, and doAggregate in the Python bindings) plus the two policies
that drive it (RenkoBarPolicy, TimeBarPolicy). Three C++ binaries answer for
most mutations:

    test_bar_aggregator            the pre-existing suite, two tests updated
    test_bar_aggregator_semantics  the acceptance tests for this fix
    test_capi_bar_aggregation      the C ABI's own copy of the close path

test_bar_close_ordering and test_sync_bar_aggregator never assert on Renko or
on a late trade, so they cannot kill any mutation here on their own -- they
are swept for compile-time and structural regressions only.

A handful of mutations live where no C++ binary can see them at all: the
Python copy of the close path (python/aggregator_bindings.h) is only ever
instantiated from python/flox_py.cpp. Those are built and run against a
throwaway venv with pybind11 and checked with the existing Python test suite
under python/tests/.

Every run is honest about the build: the mutated file's hash is printed
before and after, the target's own object files are deleted before every
rebuild so nothing can be served from cache, the rebuild output has to
contain "Building CXX" or the run is refused, and each binary runs under a
timeout. A mutation that does not compile is not a mutation and is reported
as such -- one of them (raising RenkoBarPolicy::kMaxGapBricks past the bound
the tests hard-code) is expected to fail to compile on a static_assert, which
is itself the finding.

A mutation can also be *equivalent*: syntactically a change, but behaviourally
inert because the code it edits is never instantiated for any policy actually
reachable through that path. MultiTimeframeAggregator only ever instantiates
its close path with TimeBarPolicy, TickBarPolicy or VolumeBarPolicy -- none of
which satisfies ClosesAndReopens -- so the `if constexpr (ClosesAndReopens<Policy>)`
branch in its processPolicy() is discarded at compile time for every type that
reaches it and a mutation strictly inside it cannot be observed by any test,
ever. Such mutations are marked `equivalent=True` with a reason instead of
being reported as a hole.

Usage:

    python3 scripts/mutations/bar_aggregator.py           # control, mutations, control
    python3 scripts/mutations/bar_aggregator.py --list
    python3 scripts/mutations/bar_aggregator.py --only extreme-not-pinned-to-boundary
    python3 scripts/mutations/bar_aggregator.py --skip-python

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
          -DFLOX_BUILD_TESTS=ON -DFLOX_ENABLE_BACKTEST=ON -DFLOX_BUILD_CAPI=ON

The Python build (build-py, FLOX_BUILD_PYTHON=ON against a venv with
pybind11<3.1.0) is optional; --skip-python (or its absence) is reported, not
treated as a failure -- see run_python_checks() below.
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
BUILD_PY = REPO / "build-py"
VENV_PY = REPO / ".venv-mut" / "bin" / "python"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 1800
TEST_TIMEOUT = 180

RENKO_H = "include/flox/aggregator/policies/renko_bar_policy.h"
TIME_H = "include/flox/aggregator/policies/time_bar_policy.h"
BAR_H = "include/flox/aggregator/bar.h"
POLICY_H = "include/flox/aggregator/aggregation_policy.h"
AGG_H = "include/flox/aggregator/bar_aggregator.h"
MTF_H = "include/flox/aggregator/multi_timeframe_aggregator.h"
CAPI_CPP = "src/capi/flox_capi.cpp"
PY_H = "python/aggregator_bindings.h"

T_BAR = "test_bar_aggregator"
T_SEM = "test_bar_aggregator_semantics"
T_CAPI = "test_capi_bar_aggregation"

# Never kill anything on their own (no Renko, no late-trade assertion) -- run
# on every mutation as a compile/structural safety net, per the sweep
# convention below.
SWEEP_ALWAYS = ["test_bar_close_ordering", "test_sync_bar_aggregator"]

# Files whose mutations can, in principle, be observed through the Python
# binding (python/aggregator_bindings.h includes none of these directly, but
# is templated over the same Policy types defined here).
PYTHON_RELEVANT_FILES = {RENKO_H, TIME_H, BAR_H, POLICY_H, PY_H}

PY_TEST_MODULES = ["python.tests.test_renko_gap_bricks"]


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
    edits: list[Edit] = field(default_factory=list)
    # C++ gtest binaries expected to notice, run first with gtest_filter.
    cpp_targets: list[str] = field(default_factory=list)
    gtest_filter: str | None = None
    # Whether this mutation reaches the flox_capi shared library (Renko/Time
    # policy headers do; the aggregator classes themselves do not).
    needs_capi: bool = False
    # This mutation's *only* observer is the Python binding (no C++ binary
    # even compiles the file changed).
    python_only: bool = False
    equivalent: str | None = None  # reason, or None if not claimed equivalent

    def editList(self) -> list[Edit]:
        if self.edits:
            return self.edits
        return [Edit(self.file, self.old, self.new)]

    def files(self) -> list[str]:
        seen: list[str] = []
        for e in self.editList():
            if e.file not in seen:
                seen.append(e.file)
        return seen

    def pythonRelevant(self) -> bool:
        return self.python_only or any(f in PYTHON_RELEVANT_FILES for f in self.files())


MUTATIONS: list[Mutation] = [
    # ======================================================================
    # Finding 16 -- Renko brick geometry (RenkoBarPolicy::closeAndReopen)
    # ======================================================================
    Mutation(
        name="brick-published-before-crossing-trade-is-folded",
        why="the brick is handed to emit() before update()/the boundary pin run, so the "
            "published bar is exactly the pre-fix defect: whatever the bar held before the "
            "crossing trade, not the boundary the trade actually reached",
        file=RENKO_H,
        old="""    update(trade, bar);
    bar.close = boundary(1);
    // The crossing trade is the only one that can sit past the boundary: an
    // earlier one that did would have closed this brick already. How far it
    // overshot is carried by the bricks that follow, so the extreme on the
    // side of the move is the boundary itself rather than that trade's
    // price -- otherwise a 100 -> 110 brick would report a high of 155 that
    // the next four bricks report again. The opposite side keeps whatever
    // retracement the trades actually made.
    if (sign > 0)
    {
      bar.high = bar.close;
    }
    else
    {
      bar.low = bar.close;
    }
    bar.reason = BarCloseReason::Threshold;
    emit(std::as_const(bar));""",
        new="""    emit(std::as_const(bar));
    update(trade, bar);
    bar.close = boundary(1);
    if (sign > 0)
    {
      bar.high = bar.close;
    }
    else
    {
      bar.low = bar.close;
    }
    bar.reason = BarCloseReason::Threshold;""",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.*:RenkoBarPolicyTest.*:CapiBarAggregationTest.Renko*",
    ),
    Mutation(
        name="closed-brick-close-left-at-trade-price",
        why="the boundary pin on close is dropped, so the closed brick reports whatever "
            "price the crossing trade printed at instead of the brick boundary it crossed",
        file=RENKO_H,
        old="""    update(trade, bar);
    bar.close = boundary(1);
    // The crossing trade is the only one that can sit past the boundary""",
        new="""    update(trade, bar);
    // The crossing trade is the only one that can sit past the boundary""",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.UpBrickClosesAtTheBrickBoundary:"
                     "RenkoSemanticsTest.NextBrickOpensAtTheBoundaryNotAtTheTradePrice:"
                     "RenkoBarPolicyTest.GapPastSeveralBricksSynthesizesTheMissingOnes:"
                     "CapiBarAggregationTest.RenkoBrickClosesAtTheBrickBoundary",
    ),
    Mutation(
        name="next-brick-opens-at-the-trade-price",
        why="the brick that opens next takes its open from the crossing trade's own price "
            "instead of the boundary it just closed at, shifting the whole chain off the grid",
        file=RENKO_H,
        old="""    bar.open = boundary;
    bar.close = price;""",
        new="""    bar.open = price;
    bar.close = price;""",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.NextBrickOpensAtTheBoundaryNotAtTheTradePrice:"
                     "RenkoSemanticsTest.GapLeavesTheNextBrickOpenAtTheBoundary:"
                     "RenkoSemanticsTest.DownBrickClosesAtTheBoundaryAndTheReversalOpensThere",
    ),
    Mutation(
        name="crossing-trade-counted-in-the-next-brick",
        why="update() (the trade's notional and tick) is moved past the emit and applied to "
            "the newly-opened brick instead of the one the trade completed",
        edits=[
            Edit(
                file=RENKO_H,
                old="""    update(trade, bar);
    bar.close = boundary(1);""",
                new="""    bar.close = boundary(1);""",
            ),
            Edit(
                file=RENKO_H,
                old="""    openAtBoundary(boundary(spanned), trade, bar);
  }""",
                new="""    openAtBoundary(boundary(spanned), trade, bar);
    update(trade, bar);
  }""",
            ),
        ],
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.CrossingTradeIsCountedInTheBrickItCompletes:"
                     "CapiBarAggregationTest.RenkoBrickClosesAtTheBrickBoundary",
    ),
    Mutation(
        name="reversal-brick-opens-at-the-mirrored-boundary",
        why="the brick that opens next is anchored at the boundary on the *opposite* side of "
            "the move (spanned negated) instead of the one the price actually crossed to",
        file=RENKO_H,
        old="""    openAtBoundary(boundary(spanned), trade, bar);
  }""",
        new="""    openAtBoundary(boundary(-spanned), trade, bar);
  }""",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=False,
        gtest_filter="RenkoSemanticsTest.DownBrickClosesAtTheBoundaryAndTheReversalOpensThere:"
                     "RenkoSemanticsTest.NextBrickOpensAtTheBoundaryNotAtTheTradePrice",
    ),
    Mutation(
        name="extreme-not-pinned-to-boundary",
        why="the high/low pin on the side of the move is dropped, so the closed brick reports "
            "the crossing trade's raw overshoot as its high (or low) instead of the boundary "
            "-- exactly the 'high of 155 on a 100->110 brick' the comment warns about",
        file=RENKO_H,
        old="""    // The crossing trade is the only one that can sit past the boundary: an
    // earlier one that did would have closed this brick already. How far it
    // overshot is carried by the bricks that follow, so the extreme on the
    // side of the move is the boundary itself rather than that trade's
    // price -- otherwise a 100 -> 110 brick would report a high of 155 that
    // the next four bricks report again. The opposite side keeps whatever
    // retracement the trades actually made.
    if (sign > 0)
    {
      bar.high = bar.close;
    }
    else
    {
      bar.low = bar.close;
    }
    bar.reason = BarCloseReason::Threshold;""",
        new="""    bar.reason = BarCloseReason::Threshold;""",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.*:RenkoBarPolicyTest.*:CapiBarAggregationTest.Renko*",
    ),
    # ======================================================================
    # Finding 17 -- the gap bound
    # ======================================================================
    Mutation(
        name="kmax-gap-bricks-raised-to-100000",
        why="raising the bound defeats the point of having one; kept as a compile-time "
            "check because TheCapIsDocumentedAndTheRemainderIsMarked static_asserts "
            "kMaxGapBricks against the test's own independent constant",
        file=RENKO_H,
        old="  static constexpr std::size_t kMaxGapBricks = 1024;",
        new="  static constexpr std::size_t kMaxGapBricks = 100000;",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoGapBoundTest.*:CapiBarAggregationTest.RenkoGapIsBounded",
    ),
    Mutation(
        name="capped-remainder-not-absorbed-grid-desyncs",
        why="the brick left open after a capped gap is anchored at the last *walked* boundary "
            "instead of the true trade boundary, so the un-walked remainder is never absorbed "
            "and every later trade is measured from a grid position behind the real price",
        file=RENKO_H,
        old="    openAtBoundary(boundary(spanned), trade, bar);",
        new="    openAtBoundary(boundary(walked), trade, bar);",
        cpp_targets=[T_SEM],
        needs_capi=False,
        gtest_filter="RenkoGapBoundTest.TheCappedGapStillCarriesThePriceForward",
    ),
    Mutation(
        name="absorbing-bar-without-gap-reason",
        why="the bar that absorbs the capped remainder closes with the ordinary Threshold "
            "reason, so a consumer cannot tell a capped gap from a normal close",
        file=RENKO_H,
        old="      emit(syntheticBrick(boundary(walked), boundary(spanned), tradeTs, BarCloseReason::Gap));",
        new="      emit(syntheticBrick(boundary(walked), boundary(spanned), tradeTs, BarCloseReason::Threshold));",
        cpp_targets=[T_SEM],
        needs_capi=False,
        gtest_filter="RenkoGapBoundTest.TheCapIsDocumentedAndTheRemainderIsMarked",
    ),
    Mutation(
        name="gap-reason-given-value-2",
        why="Gap is renumbered onto Forced's byte value (both compile to 2). Every test "
            "compares the *symbolic* enumerator on both sides, so a bar closed for Forced and "
            "one closed for Gap become indistinguishable at the byte level (the C ABI, and "
            "MmapBarWriter's on-disk format) without a single test noticing",
        file=BAR_H,
        old="""  Threshold = 0,  // Normal close: interval/count/volume reached
  Gap = 1,        // A price jump too wide to walk brick by brick -- see RenkoBarPolicy::kMaxGapBricks
  Forced = 2,     // Forced close: stop() called or manual flush""",
        new="""  Threshold = 0,  // Normal close: interval/count/volume reached
  Gap = 2,        // A price jump too wide to walk brick by brick -- see RenkoBarPolicy::kMaxGapBricks
  Forced = 2,     // Forced close: stop() called or manual flush""",
        cpp_targets=[T_SEM],
        needs_capi=True,
        gtest_filter="RenkoGapBoundTest.TheCapIsDocumentedAndTheRemainderIsMarked:"
                     "RenkoSemanticsTest.TradesInsideOneBrickEmitNothingUntilForced",
    ),
    # ======================================================================
    # Finding 18 -- late trades in a time bar
    # ======================================================================
    Mutation(
        name="islate-compares-against-bar-end-not-start",
        why="isLate compares the trade's aligned bucket against bar.endTime instead of "
            "bar.startTime, so a trade for the *live* bucket (aligned == startTime, which is "
            "always < endTime) is misjudged as late and dropped",
        file=TIME_H,
        old="    return alignToInterval(fromUnixNs(trade.trade.exchangeTsNs)) < bar.startTime;",
        new="    return alignToInterval(fromUnixNs(trade.trade.exchangeTsNs)) < bar.endTime;",
        cpp_targets=[T_SEM],
        needs_capi=False,
        gtest_filter="TimeBarLateTradeTest.*",
    ),
    Mutation(
        name="islate-treats-the-live-buckets-out-of-order-trade-as-late",
        why="the comparison is <= instead of <, so a trade whose bucket exactly matches the "
            "live bar (an ordinary out-of-order trade inside the current interval) is also "
            "dropped as late",
        file=TIME_H,
        old="    return alignToInterval(fromUnixNs(trade.trade.exchangeTsNs)) < bar.startTime;",
        new="    return alignToInterval(fromUnixNs(trade.trade.exchangeTsNs)) <= bar.startTime;",
        cpp_targets=[T_SEM],
        needs_capi=False,
        gtest_filter="TimeBarLateTradeTest.OutOfOrderTradeInsideTheLiveBucketIsStillFolded",
    ),
    Mutation(
        name="late-trade-count-not-incremented",
        why="the trade is still dropped, but the counter that is supposed to make the drop "
            "observable is never touched -- a silent loss with no operator-visible signal",
        file=AGG_H,
        old="""      // Dropped, not folded in: see TimeBarPolicy::isLate.
      if (_policy.isLate(trade, state.bar)) [[unlikely]]
      {
        ++_lateTradeCount;
        return;
      }""",
        new="""      // Dropped, not folded in: see TimeBarPolicy::isLate.
      if (_policy.isLate(trade, state.bar)) [[unlikely]]
      {
        return;
      }""",
        cpp_targets=[T_SEM],
        needs_capi=False,
        gtest_filter="TimeBarLateTradeTest.DroppedTradesAreCounted",
    ),
    Mutation(
        name="late-trade-folded-anyway-on-multi-timeframe",
        why="MultiTimeframeAggregator::processPolicy's isLate check is short-circuited to "
            "never fire, so its copy of the close path regresses to the pre-fix behaviour "
            "even though BarAggregator's copy is correct",
        file=MTF_H,
        old="""      if (policy.isLate(trade, state.bar)) [[unlikely]]
      {
        ++_lateTradeCount;
        return;
      }""",
        new="""      if (false && policy.isLate(trade, state.bar)) [[unlikely]]
      {
        ++_lateTradeCount;
        return;
      }""",
        cpp_targets=[T_SEM],
        needs_capi=False,
        gtest_filter="TimeBarLateTradeTest.MultiTimeframeAggregatorDropsTheLateTradeToo",
    ),
    Mutation(
        name="capi-doaggregatec-skips-the-late-trade-check",
        why="the C ABI's own copy of the close path (doAggregateC, called by every "
            "flox_aggregate_*_bars entry point and every language binding beyond Python) "
            "never gets the isLate branch at all -- no test in test_capi_bar_aggregation.cpp "
            "exercises a late trade through this path",
        file=CAPI_CPP,
        old="""    if constexpr (DetectsLateTrades<Policy>)
    {
      if (policy.isLate(trade, currentBar))
      {
        continue;
      }
    }

    if (policy.shouldClose(trade, currentBar))""",
        new="""    if (policy.shouldClose(trade, currentBar))""",
        cpp_targets=[T_CAPI],
        needs_capi=True,
        gtest_filter="CapiBarAggregationTest.*",
    ),
    Mutation(
        name="python-doaggregate-skips-the-late-trade-check",
        why="python/aggregator_bindings.h's doAggregate -- the fourth copy of the close path, "
            "reached only from the Python extension -- never gets the isLate branch; no C++ "
            "binary even compiles this file, and no Python test asserts anything about a late "
            "trade, so this is checked against the Python build directly",
        file=PY_H,
        old="""    if constexpr (flox::DetectsLateTrades<Policy>)
    {
      if (policy.isLate(trade, currentBar))
      {
        continue;
      }
    }

    if (policy.shouldClose(trade, currentBar))""",
        new="""    if (policy.shouldClose(trade, currentBar))""",
        python_only=True,
    ),
    # ======================================================================
    # Adversarial (not asked for by name; found by attacking the same code)
    # ======================================================================
    Mutation(
        name="gap-walk-off-by-one-drops-the-last-synthetic-brick",
        why="the synthetic-brick loop stops one short (i < walked instead of i <= walked), "
            "silently dropping the last whole brick a continuous price path would have "
            "produced before the trailing bar opens",
        file=RENKO_H,
        old="    for (int64_t i = 2; i <= walked; ++i)",
        new="    for (int64_t i = 2; i < walked; ++i)",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.GapLeavesTheNextBrickOpenAtTheBoundary:"
                     "RenkoBarPolicyTest.GapPastSeveralBricksSynthesizesTheMissingOnes:"
                     "CapiBarAggregationTest.RenkoRetryWithReportedCountRecoversAllBars",
    ),
    Mutation(
        name="gap-reason-set-on-every-walked-brick",
        why="every synthetic brick in the walk is marked BarCloseReason::Gap, not just the "
            "one that absorbs the capped remainder -- a consumer can no longer tell an "
            "ordinary walked brick from the one that actually lost precision",
        file=RENKO_H,
        old="      emit(syntheticBrick(boundary(i - 1), boundary(i), tradeTs, BarCloseReason::Threshold));",
        new="      emit(syntheticBrick(boundary(i - 1), boundary(i), tradeTs, BarCloseReason::Gap));",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.GapLeavesTheNextBrickOpenAtTheBoundary:"
                     "RenkoGapBoundTest.*:"
                     "RenkoBarPolicyTest.GapPastSeveralBricksSynthesizesTheMissingOnes",
    ),
    Mutation(
        name="reopened-brick-keeps-the-stale-trade-count",
        why="openAtBoundary no longer resets tradeCount, so the brick that opens next starts "
            "from whatever count the brick before it ended on instead of zero -- the count "
            "compounds forever across a whole Renko session",
        file=RENKO_H,
        old="""    bar.volume = Volume{};
    bar.buyVolume = Volume{};
    bar.tradeCount = Quantity{};""",
        new="""    bar.volume = Volume{};
    bar.buyVolume = Volume{};""",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.*:RenkoBarPolicyTest.*:CapiBarAggregationTest.Renko*",
    ),
    Mutation(
        name="reopened-brick-keeps-the-stale-volume",
        why="openAtBoundary no longer resets volume/buyVolume, so the brick that opens next "
            "starts from the notional the previous brick accumulated instead of zero",
        file=RENKO_H,
        old="""    bar.volume = Volume{};
    bar.buyVolume = Volume{};""",
        new="",
        cpp_targets=[T_BAR, T_SEM],
        needs_capi=True,
        gtest_filter="RenkoSemanticsTest.*:RenkoBarPolicyTest.*:CapiBarAggregationTest.Renko*",
    ),
    Mutation(
        name="multi-timeframe-closeandreopen-branch-is-dead-code",
        why="MultiTimeframeAggregator only ever instantiates its close path with TimeBarPolicy, "
            "TickBarPolicy or VolumeBarPolicy (PolicyTag has no Renko slot), none of which "
            "satisfies ClosesAndReopens -- so the `if constexpr (ClosesAndReopens<Policy>)` "
            "branch in processPolicy() is discarded at compile time for every type that ever "
            "reaches it. A mutation strictly inside it cannot be observed by any test that "
            "exists today, on this policy set, ever -- not a coverage hole, a dead branch",
        file=MTF_H,
        old="""        policy.closeAndReopen(trade, state.bar, [&](const Bar& bar)
                              { publishBar(slotIdx, symbol, instrument, bar); });""",
        new="""        policy.closeAndReopen(trade, state.bar, [&](const Bar& bar)
                              { publishBar(slotIdx, symbol, symbol, bar); });""",
        cpp_targets=[T_SEM],
        needs_capi=False,
        gtest_filter="TimeBarLateTradeTest.MultiTimeframeAggregatorDropsTheLateTradeToo",
        equivalent="ClosesAndReopens<Policy> is false for every Policy type "
                   "MultiTimeframeAggregator ever instantiates (Time/Tick/Volume only); this "
                   "branch is eliminated by if constexpr for all of them and never compiles "
                   "into the binary, so the edit is inert by construction.",
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


def find_objects(root: Path, *args: str) -> list[Path]:
    out = subprocess.run(["find", str(root), "-type", "f", *args],
                         capture_output=True, text=True, check=True).stdout.split()
    return [Path(p) for p in out]


def target_objects(target: str) -> list[Path]:
    return find_objects(BUILD, "-name", "*.o", "-path", f"*{target}.dir*")


class BuildFailed(Exception):
    def __init__(self, target: str, output: str):
        super().__init__(f"rebuild of {target} failed")
        self.target = target
        self.output = output


def rebuild(target: str, build_dir: Path = BUILD, requireCompile: bool = True) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(build_dir), "--target", target, "-j", BUILD_JOBS],
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
    return output


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = BIN_DIR / target
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


ALL_CPP_TARGETS = [T_BAR, T_SEM, T_CAPI] + SWEEP_ALWAYS


def control_cpp() -> bool:
    ok = True
    for target in ALL_CPP_TARGETS:
        for obj in target_objects(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target, None)
        print(f"  control {target:<28} {'green' if code == 0 else 'RED'}   {ran_line(output)}")
        ok = ok and code == 0
    if target_objects("flox_capi"):
        pass
    return ok


def python_available() -> bool:
    return (BUILD_PY / "CMakeCache.txt").is_file() and VENV_PY.is_file()


def run_python_tests() -> tuple[bool, str]:
    """Rebuild _flox_py, copy the .so into the source tree, run the existing
    Python bar tests. Returns (all_green, summary)."""
    for obj in find_objects(BUILD_PY, "-name", "*.o", "-path", "*_flox_py.dir*"):
        obj.unlink()
    out = rebuild("_flox_py", build_dir=BUILD_PY)
    compiled = sum(1 for line in out.splitlines() if "Building CXX" in line)
    print(f"  rebuilt _flox_py: {compiled} 'Building CXX' line(s)")

    so_candidates = list((BUILD_PY / "python" / "flox_py").glob("_flox_py*.so"))
    if not so_candidates:
        raise SystemExit("_flox_py rebuilt but no .so found under build-py/python/flox_py")
    dest_dir = REPO / "python" / "flox_py"
    for so in so_candidates:
        shutil.copy2(so, dest_dir / so.name)

    all_green = True
    summaries = []
    for module in PY_TEST_MODULES:
        env = dict(os.environ)
        env["PYTHONPATH"] = str(REPO / "python") + os.pathsep + env.get("PYTHONPATH", "")
        result = subprocess.run(
            [str(VENV_PY), "-m", "unittest", module, "-v"],
            capture_output=True, text=True, timeout=TEST_TIMEOUT, cwd=REPO, env=env,
        )
        text = result.stdout + result.stderr
        last = next((ln for ln in text.splitlines()[::-1] if ln.strip() in ("OK",) or
                     ln.startswith("FAILED")), text.strip().splitlines()[-1] if text.strip() else "")
        ok = result.returncode == 0
        all_green = all_green and ok
        summaries.append(f"{module}: {'OK' if ok else 'FAILED'} ({last})")
        print(f"  python {module:<45} {'green' if ok else 'RED'}")
        if not ok:
            for line in text.splitlines()[-15:]:
                print(f"    {line}")
    return all_green, "; ".join(summaries)


def control_python() -> bool:
    if not python_available():
        print("  python build not configured (build-py/ or .venv-mut/ missing) -- skipped")
        return True
    ok, summary = run_python_tests()
    print(f"  control python {'green' if ok else 'RED'}: {summary}")
    return ok


def build_cpp_targets(targets: list[str], sources: list[str]) -> None:
    removed: list[Path] = []
    for target in targets:
        removed += target_objects(target)
    if any(t == T_CAPI for t in targets) or any(f == CAPI_CPP for f in sources):
        removed += target_objects("flox_capi")
    for obj in dict.fromkeys(removed):
        obj.unlink()
    print(f"  removed {len(set(removed))} object file(s) "
          f"(0 = already deleted by the previous restore)")
    build_order = []
    if any(t == T_CAPI for t in targets) or any(f == CAPI_CPP for f in sources):
        build_order.append("flox_capi")
    build_order += targets
    for target in dict.fromkeys(build_order):
        out = rebuild(target)
        n = sum(1 for line in out.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {target}: {n} 'Building CXX' line(s)")


def run_mutation(m: Mutation, skip_python: bool) -> tuple[str, str]:
    """Returns (verdict, detail). verdict in {'killed','alive','no-compile','equivalent'}."""
    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    if m.equivalent:
        print(f"  CLAIMED EQUIVALENT: {m.equivalent}")
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
    detail_lines: list[str] = []
    try:
        cpp_targets = list(dict.fromkeys(m.cpp_targets + (SWEEP_ALWAYS if not m.equivalent else [])))
        try:
            if cpp_targets:
                build_cpp_targets(cpp_targets, m.files())
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return "no-compile", f"{e.target} failed to compile"

        filterKilled = False
        if m.gtest_filter and m.cpp_targets:
            for target in m.cpp_targets:
                code, output = run_test(target, m.gtest_filter)
                report(target, m.gtest_filter, code, output)
                if code:
                    filterKilled = True
                    verdict = "killed"
                    detail_lines.append(f"{target} --gtest_filter={m.gtest_filter}")

        if m.gtest_filter and m.cpp_targets and not filterKilled:
            print("  the named tests did not notice; falling back to the whole binaries")
        red: list[str] = []
        for target in cpp_targets:
            code, output = run_test(target, None)
            report(target, None, code, output)
            if code:
                red.append(target)
                verdict = "killed"
                detail_lines.append(f"{target} (whole binary)")
        if len(red) == 1:
            print(f"  only {red[0]} answers for this one")

        if verdict == "alive" and m.python_only:
            if skip_python:
                print("  python-only mutation and --skip-python was passed: reporting alive "
                      "by default, unverified")
                detail_lines.append("python check skipped by flag")
            elif not python_available():
                print("  NOT BUILT: build-py/ (FLOX_BUILD_PYTHON=ON) or the mutation venv is "
                      "not configured -- this mutation has no C++ observer and cannot be "
                      "evaluated without it")
                return "no-compile", "python build unavailable (NOT BUILT)"
            else:
                ok, summary = run_python_tests()
                if not ok:
                    verdict = "killed"
                    detail_lines.append(f"python: {summary}")
                else:
                    detail_lines.append(f"python stayed green: {summary}")

        if verdict == "alive" and m.pythonRelevant() and not m.python_only and not skip_python \
           and python_available():
            print("  sweeping the python build (this file is reachable from the python binding)")
            ok, summary = run_python_tests()
            if not ok:
                verdict = "killed"
                detail_lines.append(f"python sweep: {summary}")
            else:
                detail_lines.append(f"python sweep stayed green: {summary}")

        if verdict == "alive" and m.equivalent:
            verdict = "equivalent"

        if verdict == "alive":
            print("  GREEN, MUTATION SURVIVED every binary asked")
        elif verdict == "equivalent":
            print("  EQUIVALENT (see reason above) -- survived, as expected, not reported as a hole")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for source in m.files():
            for target in ALL_CPP_TARGETS:
                for obj in target_objects(target):
                    obj.unlink()
            if source == CAPI_CPP or source in (RENKO_H, TIME_H, POLICY_H, BAR_H):
                for obj in target_objects("flox_capi"):
                    obj.unlink()

    return verdict, "; ".join(detail_lines) if detail_lines else ("equivalent" if m.equivalent else "")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--skip-python", action="store_true",
                        help="do not build or run anything under build-py/")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            flag = " [python-only]" if m.python_only else ""
            eq = " [claims equivalent]" if m.equivalent else ""
            print(f"{m.name:<52}{flag}{eq} {', '.join(m.files())}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            f"  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "
            f"-DFLOX_BUILD_TESTS=ON -DFLOX_ENABLE_BACKTEST=ON -DFLOX_BUILD_CAPI=ON")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    print("control run before the mutations (C++)")
    cpp_ok = control_cpp()
    if not cpp_ok:
        raise SystemExit("the unmutated C++ tree is not green; nothing below would mean anything")

    print("\ncontrol run before the mutations (python)")
    if not args.skip_python:
        py_ok = control_python()
        if not py_ok:
            raise SystemExit("the unmutated python tree is not green; nothing below would mean "
                             "anything for the python-observed mutations")
    else:
        print("  --skip-python: not checked")

    results = [(m, *run_mutation(m, args.skip_python)) for m in selected]

    print("\ncontrol run after the mutations (C++)")
    restored = control_cpp()

    print("\nsummary")
    label = {"killed": "RED      ", "alive": "ALIVE    ", "no-compile": "NOBLD    ",
             "equivalent": "EQUIVALENT"}
    for m, verdict, detail in results:
        print(f"  {label[verdict]}  {m.name:<52} {detail}")

    survived = [m.name for m, v, _ in results if v == "alive"]
    nobuild = [m.name for m, v, _ in results if v == "no-compile"]
    equivalent = [m.name for m, v, _ in results if v == "equivalent"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived (a real hole -- report it): "
              f"{', '.join(survived)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile / could not be evaluated: "
              f"{', '.join(nobuild)}")
    if equivalent:
        print(f"{len(equivalent)} mutation(s) claimed equivalent (see reason above): "
              f"{', '.join(equivalent)}")
    if not restored:
        print("\nthe tree did not come back green after the run")

    unexplained = survived  # equivalent and no-compile are explained by definition
    return 0 if not unexplained and restored else 1


if __name__ == "__main__":
    sys.exit(main())
