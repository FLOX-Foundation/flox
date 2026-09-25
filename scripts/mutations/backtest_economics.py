#!/usr/bin/env python3
"""Mutation harness for the backtest economics fixes.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each of the six economics
fixes one piece at a time, in the source, and checks that the tests written
for them go red -- and that an unmutated tree goes green before and after.

The six fixes, and what is attacked here:

    isolated liquidation remainder  src/backtest/liquidation_engine.cpp
    funding first-tick cursor       include/flox/backtest/funding_schedule.h
    venue fee ladder on the fills   src/backtest/backtest_result.cpp,
                                    src/backtest/backtest_runner.cpp,
                                    src/backtest/venue_stack.cpp
    order latency on submit         src/backtest/simulated_executor.cpp
    walk-forward optimisation       src/backtest/walk_forward.cpp
    drawdown units in the report    include/flox/backtest/optimization_stats.h

Each mutation names the tests that are supposed to notice it. That filter runs
first; a mutation the filter does not kill is re-run against the whole of
test_backtest_economics, and a mutation still alive after that is swept across
every binary that touches the same code before it is reported green. A
survivor is therefore a survivor of everything, not just of the suite written
for the change.

A mutation flagged `equivalent` is one that is argued to be behaviour-
preserving; it is expected to survive and does not fail the run. Every other
survivor is an unexplained survivor and the script exits non-zero.

Every run is honest about the build: the mutated file's hash is printed before
and after and checked back on restore, the object of the mutated source and
every object of every target asked are deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" (and a mutated .cpp's
own object has to reappear) or the run is refused, and every build and every
binary runs under a timeout. A mutation that does not compile is not a
mutation and is reported as such.

Usage:

    python3 scripts/mutations/backtest_economics.py          # control, all, control
    python3 scripts/mutations/backtest_economics.py --list
    python3 scripts/mutations/backtest_economics.py --only liq-remainder-skipped

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
          -DFLOX_BUILD_TESTS=ON -DFLOX_NATIVE=OFF
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

LIQ = "src/backtest/liquidation_engine.cpp"
FUNDING = "include/flox/backtest/funding_schedule.h"
OPT_STATS = "include/flox/backtest/optimization_stats.h"
RESULT = "src/backtest/backtest_result.cpp"
RUNNER = "src/backtest/backtest_runner.cpp"
VENUE = "src/backtest/venue_stack.cpp"
EXEC = "src/backtest/simulated_executor.cpp"
WALK = "src/backtest/walk_forward.cpp"

PRIMARY = "test_backtest_economics"

# Every binary that answers for the code these mutations touch. A mutation
# that survives PRIMARY is run against all of these before it is called green.
SWEEP = [
    "test_liquidation_engine",
    "test_funding_schedule",
    "test_optimization_stats",
    "test_backtest",
    "test_backtest_fill_realism",
    "test_backtest_metrics",
    "test_venue_stack_calibration",
    "test_backtest_runner_venue_stack",
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
    # A mutation is usually one edit in one file (the three fields above).
    # A hole that only opens when two places move at once carries a list.
    edits: list[Edit] = field(default_factory=list)
    target: str = PRIMARY
    # The tests that are supposed to notice. Run first; the whole binary runs
    # after it as the fallback.
    gtest_filter: str | None = None
    occurrence: int = 1
    expected_occurrences: int = 1
    # Argued to be behaviour-preserving: expected to survive, and its survival
    # is not a hole. The reason is printed with the result.
    equivalent: bool = False
    equivalent_reason: str = ""

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


# ---------------------------------------------------------------------------
# 1. The isolated-margin liquidation remainder
# ---------------------------------------------------------------------------

LIQ_CREDIT = "      account.addEquity(residualEquity);"

MUTATIONS: list[Mutation] = [
    Mutation(
        name="liq-remainder-skipped",
        why="drops the credit entirely -- back to the original bug: the "
            "positive remainder of the isolated slice is erased with the leg "
            "and exists nowhere afterwards, so the account ends the tick at "
            "0.00 instead of 1,900.00",
        file=LIQ,
        old=LIQ_CREDIT,
        new="      (void)residualEquity;",
        gtest_filter="BacktestEconomics.IsolatedLiquidationReturnsSurvivingMarginToTheAccount"
                     ":BacktestEconomics.IsolatedLiquidationReturnsMarginNetOfTheLiquidationFee",
    ),
    Mutation(
        name="liq-remainder-sign-flipped",
        why="credits the negated remainder, so a surviving 1,900.00 slice "
            "debits the account by 1,900.00 instead",
        file=LIQ,
        old=LIQ_CREDIT,
        new="      account.addEquity(-residualEquity);",
        gtest_filter="BacktestEconomics.IsolatedLiquidationReturnsSurvivingMarginToTheAccount"
                     ":BacktestEconomics.IsolatedLiquidationReturnsMarginNetOfTheLiquidationFee",
    ),
    Mutation(
        name="liq-remainder-credited-twice",
        why="books the remainder onto the account twice -- the account ends "
            "at 3,800.00, money the liquidation minted out of nothing",
        file=LIQ,
        old=LIQ_CREDIT,
        new="      account.addEquity(residualEquity);\n"
            "      account.addEquity(residualEquity);",
        gtest_filter="BacktestEconomics.IsolatedLiquidationReturnsSurvivingMarginToTheAccount"
                     ":BacktestEconomics.IsolatedLiquidationReturnsMarginNetOfTheLiquidationFee",
    ),
    Mutation(
        name="liq-deficit-credited-back-to-the-account",
        why="routes the NEGATIVE branch back to the account as well: the "
            "insurance fund still pays the 1,000.00 deficit and the trader is "
            "handed the same 1,000.00 as a credit, so a blown-through "
            "isolated leg ends profitable",
        file=LIQ,
        old="      result.deficit += -residualEquity;",
        new="      result.deficit += -residualEquity;\n"
            "      account.addEquity(-residualEquity);",
        gtest_filter="BacktestEconomics.IsolatedLiquidationSendsANegativeRemainderToInsurance",
    ),
    Mutation(
        name="liq-remainder-goes-to-insurance-instead",
        why="sends the surviving margin to the insurance fund rather than "
            "back to the trader: the account still ends at 0.00 and the fund "
            "grows by money that was never its own",
        file=LIQ,
        old=LIQ_CREDIT,
        new="      result.deficit -= residualEquity;",
        gtest_filter="BacktestEconomics.IsolatedLiquidationReturnsSurvivingMarginToTheAccount"
                     ":BacktestEconomics.IsolatedLiquidationReturnsMarginNetOfTheLiquidationFee",
    ),
    Mutation(
        name="liq-remainder-ignores-the-close",
        why="credits the posted slice gross instead of net: the realised "
            "loss and the liquidation fee are not deducted, so the account "
            "gets 2,000.00 back on a close that cost it 124.75",
        file=LIQ,
        old=LIQ_CREDIT,
        new="      account.addEquity(filledEquity);",
        gtest_filter="BacktestEconomics.IsolatedLiquidationReturnsSurvivingMarginToTheAccount"
                     ":BacktestEconomics.IsolatedLiquidationReturnsMarginNetOfTheLiquidationFee",
    ),

    # -----------------------------------------------------------------------
    # 2. The funding first-tick cursor
    # -----------------------------------------------------------------------
    Mutation(
        name="funding-seed-not-applied",
        why="back to the original bug: the first tick leaves the cursor at "
            "the epoch, so a 2026 timestamp settles 61,362 8h boundaries "
            "against the position held on that one tick",
        file=FUNDING,
        old="      const int64_t previousBoundary = (nowNs / _intervalNs) * _intervalNs;\n"
            "      _lastTickNs = std::max(_lastTickNs, previousBoundary - _intervalNs);",
        new="      (void)nowNs;",
        gtest_filter="BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce",
    ),
    Mutation(
        name="funding-seeds-from-the-next-boundary",
        why="seeds the cursor one interval into the FUTURE instead of one "
            "into the past, so the opening tick pays nothing and the whole "
            "cadence afterwards is short by one settlement",
        file=FUNDING,
        old="      _lastTickNs = std::max(_lastTickNs, previousBoundary - _intervalNs);",
        new="      _lastTickNs = std::max(_lastTickNs, previousBoundary + _intervalNs);",
        gtest_filter="BacktestEconomics.FundingCadenceAfterTheFirstTickControl"
                     ":BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce",
    ),
    Mutation(
        name="funding-seed-off-by-one-interval",
        why="seeds two intervals back instead of one, so the first tick "
            "settles two boundaries against a position that has been open "
            "for one tick",
        file=FUNDING,
        old="      _lastTickNs = std::max(_lastTickNs, previousBoundary - _intervalNs);",
        new="      _lastTickNs = std::max(_lastTickNs, previousBoundary - 2 * _intervalNs);",
        gtest_filter="BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce",
    ),
    Mutation(
        name="funding-seed-without-max",
        why="drops the std::max, so the seed can drag an already-advanced "
            "cursor BACKWARDS -- a schedule driven from a small base, or one "
            "explicitly positioned before its first tick, re-settles "
            "boundaries it has already paid",
        file=FUNDING,
        old="      _lastTickNs = std::max(_lastTickNs, previousBoundary - _intervalNs);",
        new="      _lastTickNs = previousBoundary - _intervalNs;",
        gtest_filter="BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce"
                     ":BacktestEconomics.FundingCadenceAfterTheFirstTickControl",
    ),
    Mutation(
        name="funding-seed-from-now-not-the-boundary",
        why="seeds from `now - interval` rather than from the boundary grid, "
            "so the cursor lands off the 8h grid instead of on it",
        file=FUNDING,
        old="      const int64_t previousBoundary = (nowNs / _intervalNs) * _intervalNs;",
        new="      const int64_t previousBoundary = nowNs;",
        gtest_filter="BacktestEconomics.FundingCadenceAfterTheFirstTickControl"
                     ":BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce",
        equivalent=True,
        equivalent_reason=(
            "written as a bug, argued out as equivalent after it survived. "
            "The cursor is never read directly: the emitting loop re-grids it "
            "(`((_lastTickNs / _intervalNs) + 1) * _intervalNs`), and both "
            "seeds floor to the same grid cell -- "
            "floor((now - interval)/interval) == floor(now/interval) - 1 == "
            "floor(previousBoundary/interval) - 1 -- so the first boundary "
            "emitted is identical. The `t <= _lastTickNs` skip cannot "
            "separate them either, because previousBoundary > now - interval "
            "holds for every now, so no boundary is skipped under the higher "
            "cursor. The two spans differ (interval vs up to 2*interval) but "
            "both are astronomically below kMaxBoundariesPerTick, and "
            "std::max resolves the same way against the only pre-first-tick "
            "value _lastTickNs can hold (0, there being no setter). The tick "
            "leaves _lastTickNs = nowNs on both paths, so lastTickNs() cannot "
            "observe it. A real off-grid-seed bug would have to survive the "
            "re-gridding, which this one does not."),
    ),
    Mutation(
        name="funding-seeded-flag-never-latches",
        why="leaves _seeded false, so EVERY tick re-seeds the cursor to the "
            "boundary before `now`: the schedule can never settle more than "
            "the last boundary and a 24h gap pays one interval, not three",
        file=FUNDING,
        old="      _seeded = true;\n"
            "      const int64_t previousBoundary",
        new="      _seeded = false;\n"
            "      const int64_t previousBoundary",
        gtest_filter="BacktestEconomics.FundingCadenceAfterTheFirstTickControl",
    ),
    Mutation(
        name="funding-backstop-clamp-removed",
        why="removes the later-tick backstop: a resumed run or a gap in the "
            "tape hands the schedule an unbounded jump and it emits an "
            "unbounded payment list rather than clamping and warning",
        file=FUNDING,
        old="        _lastTickNs = nowNs - kMaxBoundariesPerTick * _intervalNs;",
        new="        (void)span;",
        gtest_filter="BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce"
                     ":BacktestEconomics.FundingCadenceAfterTheFirstTickControl",
    ),
    Mutation(
        name="funding-backstop-clamp-reversed",
        why="inverts the backstop's comparison, so the clamp fires on every "
            "NORMAL tick and drags the cursor 100,000 intervals into the "
            "past instead of guarding against a jump",
        file=FUNDING,
        old="      if (span / _intervalNs > kMaxBoundariesPerTick)",
        new="      if (span / _intervalNs < kMaxBoundariesPerTick)",
        gtest_filter="BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce"
                     ":BacktestEconomics.FundingCadenceAfterTheFirstTickControl",
    ),
    Mutation(
        name="funding-previous-boundary-by-modulo",
        why="rewrites the boundary floor as `nowNs - nowNs % _intervalNs` "
            "instead of `(nowNs / _intervalNs) * _intervalNs`",
        file=FUNDING,
        old="      const int64_t previousBoundary = (nowNs / _intervalNs) * _intervalNs;",
        new="      const int64_t previousBoundary = nowNs - (nowNs % _intervalNs);",
        gtest_filter="BacktestEconomics.FirstFundingTickOnARealTimestampSettlesAtMostOnce"
                     ":BacktestEconomics.FundingCadenceAfterTheFirstTickControl",
        equivalent=True,
        equivalent_reason=(
            "C++ integer division truncates toward zero and % satisfies "
            "(a/b)*b + a%b == a for every representable pair, so "
            "a - a%b == (a/b)*b identically, for negative nowNs as well as "
            "positive. _intervalNs > 0 is already guarded by the enclosing "
            "condition, so the only other way the two could differ -- "
            "division by zero -- cannot be reached."),
    ),

    # -----------------------------------------------------------------------
    # 3. The venue fee ladder reaching the fills
    # -----------------------------------------------------------------------
    Mutation(
        name="fee-schedule-not-installed-by-venue-wiring",
        why="the canned venue stack stops handing its ladder to the "
            "executor, so a run on that stack has no schedule to price "
            "against and falls back to the flat BacktestConfig::feeRate",
        file=VENUE,
        old="  a.executor->setFeeSchedule(a.fees.get());",
        new="  a.executor->setFeeSchedule(nullptr);",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-schedule-not-forwarded-to-the-result",
        why="result() stops forwarding the executor's ladder, so the fills "
            "are priced at the flat config rate even though the stack is "
            "wired -- the exact shape of the bug the fix closed",
        file=RUNNER,
        old="  BacktestResult res(_config, sim().fills().size());\n"
            "  attachFeeSchedule(res);\n"
            "  for (const auto& fill : sim().fills())",
        new="  BacktestResult res(_config, sim().fills().size());\n"
            "  for (const auto& fill : sim().fills())",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-schedule-not-forwarded-by-extract-result",
        why="the same drop on the move-out path: extractResult() builds a "
            "result with no ladder, so a run whose fills are extracted "
            "rather than copied is billed at the flat rate",
        file=RUNNER,
        old="  BacktestResult res(_config, sim().fills().size());\n"
            "  attachFeeSchedule(res);\n"
            "  auto fills = sim().extractFills();",
        new="  BacktestResult res(_config, sim().fills().size());\n"
            "  auto fills = sim().extractFills();",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-ladder-copy-stays-bound-to-the-account",
        why="leaves the result's copy bound to the venue account, so the "
            "replay writes its own fills into the live 30-day counter and "
            "reads the tier back out of it: pricing a result mutates the "
            "venue, and a second result() prices the same fills differently",
        file=RESULT,
        old="  _fees->clearAccountBinding();",
        new="  (void)0;",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier"
                     ":BacktestEconomics.VenueFeeScheduleResolvesTheVolumeTierControl",
    ),
    Mutation(
        name="fee-seeded-notional-dropped",
        why="the pre-run 30-day notional is not carried into the copy, so a "
            "VIP-8 account is priced from zero volume: the run pays tier 0 "
            "(4.0 bps, 80.00) instead of tier 8 (2.1 bps, 42.00)",
        file=RESULT,
        old="  _feeBaseNotional30d =\n"
            "      (schedule.boundAccount() != nullptr) ? schedule.rollingNotional30d() : 0.0;",
        new="  _feeBaseNotional30d = 0.0;",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-seeded-notional-stamped-at-the-epoch",
        why="pushes the pre-run volume in at timestamp 0 instead of at the "
            "first fill's timestamp, so it sits outside the 30-day window "
            "and is evicted before it can lift the tier",
        file=RESULT,
        old="        _fees->recordFill(tsNs.raw(), _feeBaseNotional30d);",
        new="        _fees->recordFill(0, _feeBaseNotional30d);",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-tier-never-climbs",
        why="the replay stops pushing its own fills into the copy, so the "
            "tier is frozen at whatever the pre-run notional resolved to: a "
            "run that trades its way up a ladder keeps paying the opening "
            "tier for its whole length",
        file=RESULT,
        old="    _fees->recordFill(tsNs.raw(), notional.toDouble());",
        new="    (void)notional;",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-base-pushed-on-every-fill",
        why="drops the once-only latch, so the pre-run notional is pushed in "
            "again on every single fill: a VIP account's volume is counted "
            "as many times as the run has fills and the tier is inflated",
        file=RESULT,
        old="      _feeBaseApplied = true;\n",
        new="",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-precedence-ladder-outranks-the-explicit-fee",
        why="swaps the precedence: a caller that replaced the fee model "
            "outright with a fixed per-trade fee (usePercentageFee == false) "
            "is overridden by the venue ladder instead of winning",
        file=RESULT,
        old="  if (!_config.usePercentageFee)\n"
            "  {\n"
            "    return Volume::fromDouble(_config.fixedFeePerTrade);\n"
            "  }\n"
            "\n"
            "  const Volume notional = price * qty;\n",
        new="  const Volume notional = price * qty;\n"
            "\n"
            "  if (!_config.usePercentageFee && !(_fees.has_value() && _fees->tierCount() > 0))\n"
            "  {\n"
            "    return Volume::fromDouble(_config.fixedFeePerTrade);\n"
            "  }\n",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-per-side-rate-ignored-by-the-ladder",
        why="prices every fill at the ladder's MAKER rate regardless of "
            "Fill::isMaker, so a taker round trip on a Regular account pays "
            "2.0 bps (40.00) instead of 4.0 bps (80.00)",
        file=RESULT,
        old="    const double fee = _fees->feeFor(tsNs.raw(), notional.toDouble(), isMaker);",
        new="    const double fee = _fees->feeFor(tsNs.raw(), notional.toDouble(), true);",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),
    Mutation(
        name="fee-empty-ladder-still-consulted",
        why="drops the tierCount() guard, so a venue whose schedule carries "
            "no tiers at all is consulted anyway and the flat config rate is "
            "never reached -- a fee of whatever an empty ladder returns",
        file=RESULT,
        old="  if (_fees.has_value() && _fees->tierCount() > 0)",
        new="  if (_fees.has_value())",
        gtest_filter="BacktestEconomics.RunOnAVenueStackPaysThatStacksFeeTier",
    ),

    # -----------------------------------------------------------------------
    # 4. Order latency on submit
    # -----------------------------------------------------------------------
    Mutation(
        name="latency-not-applied-by-apply-config",
        why="applyConfig stops installing BacktestConfig::latency, so the "
            "field exists and is ignored -- every backtest is back to zero "
            "order latency",
        file=EXEC,
        old="  setLatencyModel(config.latency);",
        new="  setLatencyModel(nullptr);",
        gtest_filter="BacktestEconomics.ConstantOrderLatencyDelaysTheFill",
    ),
    Mutation(
        name="latency-subtracted-from-the-deferral",
        why="subtracts the wire delay instead of adding it, so a 5 ms "
            "latency produces a non-positive deferral and the order fills "
            "synchronously at submit time",
        file=EXEC,
        old="  const int64_t deferNs =\n"
            "      sampleOrderLatency() +",
        new="  const int64_t deferNs =\n"
            "      -sampleOrderLatency() +",
        gtest_filter="BacktestEconomics.ConstantOrderLatencyDelaysTheFill",
    ),
    Mutation(
        name="latency-branch-on-the-model-not-the-sample",
        why="branches on whether a model is attached rather than on the "
            "sampled total, so a model that draws zero stops reproducing the "
            "instant baseline: the fill is deferred to the next event",
        file=EXEC,
        old="  if (deferNs > 0)",
        new="  if (deferNs > 0 || _latency != nullptr)",
        gtest_filter="BacktestEconomics.ConstantOrderLatencyDelaysTheFill"
                     ":BacktestEconomics.NoLatencyModelFillsAtSubmitTimeControl",
    ),
    Mutation(
        name="latency-deadline-ignores-the-delay",
        why="stamps the pending submission's ack deadline at `now`, so the "
            "delay is computed and then thrown away -- the order is "
            "accepted on the very next finalize and never waits the wire",
        file=EXEC,
        old="      PendingSubmission{.ackAtNs = now + delayNs, .order = order});",
        new="      PendingSubmission{.ackAtNs = now, .order = order});",
        gtest_filter="BacktestEconomics.ConstantOrderLatencyDelaysTheFill",
    ),
    Mutation(
        name="latency-drops-the-submit-ack-component",
        why="the venue's own submit-ack latency stops composing with the "
            "wire model: an ack profile configured without a latency model "
            "no longer defers anything",
        file=EXEC,
        old="      sampleOrderLatency() +\n"
            "      (_submitAckDist.medianNs() > 0 ? sampleSubmitAckLatency() : 0);",
        new="      sampleOrderLatency();",
        gtest_filter="BacktestEconomics.ConstantOrderLatencyDelaysTheFill"
                     ":BacktestEconomics.NoLatencyModelFillsAtSubmitTimeControl",
    ),
    Mutation(
        name="latency-drawn-once-per-process-not-per-order",
        why="caches the first draw in a function-local static, so a "
            "stochastic model is sampled once for the life of the process "
            "and every later order in every later run reuses that one draw",
        file=EXEC,
        old="  return std::max<int64_t>(0, _latency->orderDelay());",
        new="  static const int64_t drawn = std::max<int64_t>(0, _latency->orderDelay());\n"
            "  return drawn;",
        gtest_filter="BacktestEconomics.ConstantOrderLatencyDelaysTheFill",
    ),
    Mutation(
        name="latency-negative-draw-not-clamped",
        why="removes the clamp, so a model that draws a negative order delay "
            "(a jittered profile whose jitter exceeds its median) produces a "
            "negative deferral -- an order arriving before it was sent",
        file=EXEC,
        old="  return std::max<int64_t>(0, _latency->orderDelay());",
        new="  return _latency->orderDelay();",
        gtest_filter="BacktestEconomics.ConstantOrderLatencyDelaysTheFill"
                     ":BacktestEconomics.NoLatencyModelFillsAtSubmitTimeControl",
    ),

    # -----------------------------------------------------------------------
    # 5. Walk-forward optimisation
    # -----------------------------------------------------------------------
    Mutation(
        name="wf-grid-ignored-for-a-parameterised-factory",
        why="inverts the guard in gridPoints(), so the cartesian product is "
            "built only for the factory shape that cannot receive it: a "
            "parameterised factory gets the single empty point and the grid "
            "is never searched",
        file=WALK,
        old="  if (!_factoryTakesParams)",
        new="  if (_factoryTakesParams)",
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-cartesian-product-drops-the-last-axis",
        why="stops one axis short of the end, so the last parameter is never "
            "varied -- with a one-axis grid nothing is searched at all, and "
            "with several the last one silently takes whatever the strategy "
            "defaults to",
        file=WALK,
        old="  for (const auto& axis : _grid)\n  {",
        new="  for (std::size_t axisIdx = 0; axisIdx + 1 < _grid.size(); ++axisIdx)\n"
            "  {\n    const auto& axis = _grid[axisIdx];",
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-empty-axis-cancels-the-fold",
        why="an empty axis is multiplied through instead of skipped, so one "
            "unset parameter leaves no grid points at all and the fold "
            "evaluates nothing",
        file=WALK,
        old="      FLOX_LOG_WARN(\"WalkForwardRunner: empty parameter axis skipped\");\n"
            "      continue;",
        new="      FLOX_LOG_WARN(\"WalkForwardRunner: empty parameter axis skipped\");\n"
            "      points.clear();\n"
            "      return points;",
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-ranking-picks-the-worst-point",
        why="reverses the in-sample ranking, so the fold hands the WORST "
            "grid point to the out-of-sample run: on a rising ramp the "
            "shortest hold wins instead of the longest",
        file=WALK,
        old="    if (!best.selected || stats.netPnl > best.stats.netPnl)",
        new="    if (!best.selected || stats.netPnl < best.stats.netPnl)",
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-tie-broken-to-the-later-point",
        why="a tie on net PnL now goes to the LATER grid point rather than "
            "the earlier one, so the selection depends on the order the "
            "product happens to enumerate in and is no longer the "
            "deterministic rule the fix documents",
        file=WALK,
        old="    if (!best.selected || stats.netPnl > best.stats.netPnl)",
        new="    if (!best.selected || stats.netPnl >= best.stats.netPnl)",
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-ranking-short-circuit-reordered",
        why="evaluates the comparison before the have-a-best flag in the "
            "same || expression",
        file=WALK,
        old="    if (!best.selected || stats.netPnl > best.stats.netPnl)",
        new="    if (stats.netPnl > best.stats.netPnl || !best.selected)",
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
        equivalent=True,
        equivalent_reason=(
            "`a || b` and `b || a` select the same branch whenever neither "
            "operand has a side effect, and neither does here: best.selected "
            "is a plain bool member and the comparison reads two values. The "
            "only thing the original order guaranteed was that the comparison "
            "is not reached on the first iteration, and on that iteration "
            "best.stats is value-initialised, so comparing against it is "
            "well-defined -- it just cannot change the outcome, because "
            "!best.selected is still true and still selects the point."),
    ),
    Mutation(
        name="wf-skipped-grid-point-still-wins",
        why="a factory that returns nullptr for a grid point (the documented "
            "way to skip one) is recorded as the best anyway, with the "
            "zeroed stats of a window that was never run",
        file=WALK,
        old="    IStrategy* strategy = factory(foldIndex, point);\n"
            "    if (strategy == nullptr)\n"
            "    {\n"
            "      continue;\n"
            "    }",
        new="    IStrategy* strategy = factory(foldIndex, point);\n"
            "    if (strategy == nullptr)\n"
            "    {\n"
            "      best.params = point;\n"
            "      best.selected = true;\n"
            "      continue;\n"
            "    }",
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-sliding-trains-on-the-test-slice",
        why="the sliding fold evaluates the grid on the OUT-OF-SAMPLE bars: "
            "the selection sees the window it is supposed to be validated "
            "on, which is the look-ahead the whole construction exists to "
            "prevent",
        file=WALK,
        old="            return runWindow(_backtestConfig, strategy, bars, f.trainStartBar,\n"
            "                             f.trainEndBar);",
        new="            return runWindow(_backtestConfig, strategy, bars, f.testStartBar,\n"
            "                             f.testEndBar);",
        occurrence=2,
        expected_occurrences=2,
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-sliding-oos-ignores-the-winner",
        why="the sliding fold builds its out-of-sample strategy from the "
            "FIRST grid point instead of the winner, so the train window's "
            "ranking never reaches the test window",
        file=WALK,
        old="      IStrategy* testStrat = _factory(f.foldIndex, winner.params);\n"
            "      if (!testBuildDeclined(f, testStrat))\n"
            "      {\n"
            "        f.testStats = runWindow(_backtestConfig, testStrat, bars,",
        new="      IStrategy* testStrat = _factory(f.foldIndex, points.front());\n"
            "      if (!testBuildDeclined(f, testStrat))\n"
            "      {\n"
            "        f.testStats = runWindow(_backtestConfig, testStrat, bars,",
        occurrence=2,
        expected_occurrences=2,
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-sliding-train-stats-zeroed",
        why="the fold reports empty train stats instead of the winning "
            "point's, so the in-sample half of every reported fold is blank "
            "and the degradation from train to test is unmeasurable",
        file=WALK,
        old="      f.trainStats = winner.stats;\n\n"
            "      // Out of sample on the winning point, built fresh so the test window\n"
            "      // starts from clean strategy state.\n"
            "      IStrategy* testStrat = _factory(f.foldIndex, winner.params);\n"
            "      if (!testBuildDeclined(f, testStrat))\n"
            "      {\n"
            "        f.testStats = runWindow(",
        new="      f.trainStats = BacktestStats{};\n\n"
            "      // Out of sample on the winning point, built fresh so the test window\n"
            "      // starts from clean strategy state.\n"
            "      IStrategy* testStrat = _factory(f.foldIndex, winner.params);\n"
            "      if (!testBuildDeclined(f, testStrat))\n"
            "      {\n"
            "        f.testStats = runWindow(",
        occurrence=2,
        expected_occurrences=2,
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-anchored-oos-ignores-the-winner",
        why="the same drop on the ANCHORED branch of the bar-price overload: "
            "the grid is searched in sample and the test window is run on "
            "the first grid point regardless",
        file=WALK,
        old="      IStrategy* testStrat = _factory(f.foldIndex, winner.params);\n"
            "      if (!testBuildDeclined(f, testStrat))\n"
            "      {\n"
            "        f.testStats = runWindow(_backtestConfig, testStrat, bars,",
        new="      IStrategy* testStrat = _factory(f.foldIndex, points.front());\n"
            "      if (!testBuildDeclined(f, testStrat))\n"
            "      {\n"
            "        f.testStats = runWindow(_backtestConfig, testStrat, bars,",
        occurrence=1,
        expected_occurrences=2,
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),
    Mutation(
        name="wf-barevent-overload-ignores-the-winner",
        why="the same drop on both branches of the BarEvent overload -- the "
            "overload a run driven by real OHLCV bar events takes",
        edits=[
            Edit(
                file=WALK,
                old="      IStrategy* testStrat = _factory(f.foldIndex, winner.params);\n"
                    "      if (!testBuildDeclined(f, testStrat))\n"
                    "      {\n"
                    "        f.testStats = runWindowBars(_backtestConfig, testStrat, "
                    "bars,",
                new="      IStrategy* testStrat = _factory(f.foldIndex, points.front());\n"
                    "      if (!testBuildDeclined(f, testStrat))\n"
                    "      {\n"
                    "        f.testStats = runWindowBars(_backtestConfig, testStrat, "
                    "bars,",
                occurrence=1,
                expected_occurrences=2,
            ),
            Edit(
                file=WALK,
                old="      IStrategy* testStrat = _factory(f.foldIndex, winner.params);\n"
                    "      if (!testBuildDeclined(f, testStrat))\n"
                    "      {\n"
                    "        f.testStats = runWindowBars(_backtestConfig, testStrat, "
                    "bars,",
                new="      IStrategy* testStrat = _factory(f.foldIndex, points.front());\n"
                    "      if (!testBuildDeclined(f, testStrat))\n"
                    "      {\n"
                    "        f.testStats = runWindowBars(_backtestConfig, testStrat, "
                    "bars,",
                occurrence=1,
                expected_occurrences=1,
            ),
        ],
        gtest_filter="BacktestEconomics.WalkForwardRunsOutOfSampleOnTheInSampleWinner",
    ),

    # -----------------------------------------------------------------------
    # 6. Drawdown units in the optimisation report
    # -----------------------------------------------------------------------
    Mutation(
        name="dd-report-row-scaled-to-percent-twice",
        why="restores the second `* 100` in the report's Drawdown column: a "
            "10 percent drawdown is published as 1000.00%, ten times the "
            "size of the account whose equity curve is on the same page",
        file=OPT_STATS,
        old="           << res.maxDrawdownPct() << \"% | \"",
        new="           << (res.maxDrawdownPct() * 100) << \"% | \"",
        gtest_filter="BacktestEconomics.OptimizationReportPrintsDrawdownAsPercentOnce",
    ),
    Mutation(
        name="dd-print-summary-scaled-to-percent-twice",
        why="restores the second `* 100` on the line printSummary LOGS, "
            "rather than the one generateReport writes: the same drawdown is "
            "reported as 1000.00% to anyone reading the log",
        file=OPT_STATS,
        old="                                  << \" DD=\" << best->maxDrawdownPct() << \"%\"",
        new="                                  << \" DD=\" << (best->maxDrawdownPct() * 100) << \"%\"",
        gtest_filter="BacktestEconomics.OptimizationReportPrintsDrawdownAsPercentOnce",
    ),
    Mutation(
        name="dd-win-rate-scaling-dropped-with-it",
        why="takes the `* 100` off winRate as well -- the over-correction "
            "the drawdown fix had to avoid: winRate really is a fraction, so "
            "a 50 percent win rate is now printed as 0.50%",
        file=OPT_STATS,
        old="           << (res.winRate() * 100) << \"% | \"",
        new="           << res.winRate() << \"% | \"",
        gtest_filter="BacktestEconomics.OptimizationReportPrintsDrawdownAsPercentOnce",
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


def source_objects(source: str) -> list[Path]:
    """The library object compiled from a mutated .cpp.

    `find build -name '<file>.o' -delete`, spelled out so the paths can be
    printed and so their reappearance after the build can be asserted. A
    mutated header has no object of its own; the generated dependency files
    are what force its includers to recompile, and the "Building CXX" count
    is the proof that they did.
    """
    if source.endswith((".h", ".inl", ".hpp")):
        return []
    return find_objects("-name", f"{Path(source).name}.o")


class BuildFailed(Exception):
    def __init__(self, target: str, output: str):
        super().__init__(f"rebuild of {target} failed")
        self.target = target
        self.output = output


def rebuild(target: str, requireCompile: bool = True) -> str:
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


def build_for(target: str, sources: list[str]) -> None:
    """Delete every object that could hide the mutation, then rebuild."""
    removed: list[Path] = []
    for source in sources:
        removed += source_objects(source)
    removed += target_objects(target)
    for obj in dict.fromkeys(removed):
        obj.unlink()
    print(f"  removed {len(set(removed))} object file(s)")

    output = rebuild(LIB, requireCompile=False)
    output += rebuild(target, requireCompile=True)
    compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
    print(f"  rebuilt {LIB} + {target}: {compiled} 'Building CXX' line(s)")

    # A mutated .cpp has exactly one object; it was deleted above, so its
    # reappearance is the proof that this build compiled the mutated text and
    # did not link something stale.
    for source in sources:
        if source.endswith((".h", ".inl", ".hpp")):
            continue
        if not source_objects(source):
            raise SystemExit(
                f"the object for {source} did not come back after the rebuild; "
                f"the binary would be stale, so the run is refused")


def run_mutation(m: Mutation) -> str:
    """'killed', 'alive', 'equivalent' or 'no-compile'."""
    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    print(f"\n[{m.name}]" + ("  (argued equivalent)" if m.equivalent else ""))
    print(f"  {m.why}")
    if m.equivalent:
        print(f"  equivalent because: {m.equivalent_reason}")
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
        try:
            build_for(m.target, m.files())
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return "no-compile"

        # The tests that are supposed to notice, first.
        if m.gtest_filter:
            code, output = run_test(m.target, m.gtest_filter)
            report(m.target, m.gtest_filter, code, output)
            if code:
                verdict = "killed"

        # The whole binary as the fallback: a mutation the named tests miss
        # may still be caught by a neighbour in the same suite, and that is
        # worth knowing before the sweep.
        if verdict == "alive":
            if m.gtest_filter:
                print("  the named tests did not notice; falling back to the whole binary")
            code, output = run_test(m.target, None)
            report(m.target, None, code, output)
            if code:
                verdict = "killed"

        # Still alive: sweep every binary that touches the same code.
        if verdict == "alive":
            print("  sweeping the related binaries")
            try:
                for target in SWEEP:
                    if target == m.target:
                        continue
                    # The mutation is in the library (or a header the library
                    # includes), already recompiled above, so these binaries
                    # only have to be linked again -- except for a test-only
                    # header, which recompiles the test's own object.
                    out = rebuild(target, requireCompile=False)
                    if "Linking CXX" not in out and "Building CXX" not in out:
                        raise SystemExit(
                            f"{target} was neither compiled nor linked after the "
                            f"library changed; the binary would be stale:\n{out[-800:]}")
            except BuildFailed as e:
                print(f"  sweep build of {e.target} failed")
                print("\n".join(e.output.splitlines()[-25:]))
                return "no-compile"
            for target in SWEEP:
                if target == m.target:
                    continue
                code, output = run_test(target, None)
                report(target, None, code, output)
                if code:
                    verdict = "killed"

        if verdict == "alive":
            if m.equivalent:
                verdict = "equivalent"
                print("  GREEN as argued -- behaviour-preserving, not a hole")
            else:
                print("  GREEN, MUTATION SURVIVED every binary asked")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for source in m.files():
            for obj in source_objects(source):
                obj.unlink()
        for obj in target_objects(m.target):
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
            flag = " [equivalent]" if m.equivalent else ""
            print(f"{m.name:<52} {', '.join(m.files())}{flag}")
        print(f"\n{len(MUTATIONS)} mutations")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            f"  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "
            f"-DFLOX_BUILD_TESTS=ON -DFLOX_NATIVE=OFF")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = sorted({m.target for m in selected})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "equivalent": "EQUIV",
             "no-compile": "NOBLD"}
    for m, verdict in results:
        print(f"  {label[verdict]}  {m.name:<52} {', '.join(m.files())}")
    survived = [m.name for m, v in results if v == "alive"]
    equivalent = [m.name for m, v in results if v == "equivalent"]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    if equivalent:
        print(f"\n{len(equivalent)} mutation(s) survived as argued equivalent: "
              f"{', '.join(equivalent)}")
    if survived:
        print(f"\n{len(survived)} unexplained survivor(s) -- holes in the tests: "
              f"{', '.join(survived)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and not nobuild and restored else 1


if __name__ == "__main__":
    sys.exit(main())
