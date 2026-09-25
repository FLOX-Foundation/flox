#!/usr/bin/env python3
"""Mutation harness for the venue money-in-fixed-point fix (fees, funding,
rendering).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the fix one piece at a time,
in the source, and checks that the acceptance tests -- and a curated set of
regression suites that exercise the same code from other angles -- go red.

Three acceptance binaries answer for the three findings the fix covers:

    test_venue_fee_fixed_point       fees (finding 18)
    test_venue_funding_fixed_point   funding (finding 19)
    test_venue_money_rendering       control-plane / Prometheus text (finding 20)

Each mutation also runs a curated regression set for the area it touches --
other tests that call the same functions with different data -- plus
test_venue_golden_replay every time, per the task's own instruction to
include it in the sweep of any survivor. A mutation is killed if ANY of its
curated targets goes red.

A mutation that survives its curated targets is not taken at face value: the
harness then rebuilds and runs the ENTIRE venue-lite ctest suite (all ~105
binaries) against the still-mutated tree before calling it a genuine
survivor. If some other test the curated set did not think to ask catches
it, the mutation is reported as killed-by-full-suite, naming what caught it,
rather than being reported as a hole that is not actually there.

Every run is honest about the build: the mutated file's hash is printed
before and after, every curated target's object files are deleted so nothing
can be served from cache, the rebuild output has to contain "Building CXX" or
the run is refused, and each binary runs under a timeout. A mutation that
does not compile is not a mutation.

A mutation may carry `equivalent_reason`: a mutation that provably cannot
change any observable value under the arithmetic actually exercised is still
run and still reported, but does not fail the overall exit code.

Usage:

    python3 scripts/mutations/venue_money.py            # everything
    python3 scripts/mutations/venue_money.py --list
    python3 scripts/mutations/venue_money.py --only feerate-truncate
    python3 scripts/mutations/venue_money.py --skip-full-suite   # curated targets only

The build directory is expected to be configured already:

    cmake --preset venue-lite
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
BUILD = REPO / "build-venue-lite"
BIN_DIR = BUILD / "venue"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 120
CTEST_TIMEOUT = 300

FEES_H = "venue/include/flox-venue/engine/fees.h"
LEDGER_H = "venue/include/flox-venue/ledger.h"
MESSAGES_H = "venue/include/flox-venue/messages.h"
CLEARING_H = "venue/include/flox-venue/engine/clearing.h"
CROSS_MARGIN_H = "venue/include/flox-venue/cross_margin.h"
FIXED_POINT_TEXT_H = "venue/include/flox-venue/fixed_point_text.h"
CONTROL_API_H = "venue/include/flox-venue/control_api.h"
PROMETHEUS_H = "venue/include/flox-venue/prometheus.h"

FEE = "test_venue_fee_fixed_point"
FUNDING = "test_venue_funding_fixed_point"
RENDER = "test_venue_money_rendering"
GOLDEN = "test_venue_golden_replay"

# Regression: other tests that exercise the same functions with different
# data, found by grepping the venue test tree for the call sites the fix
# touched (addTier/currentBps for fees, applyFunding for funding, the
# control-plane/Prometheus renderers for text).
FEE_REGRESSION = [
    "test_venue_engine_composition",  # engine::Fees directly: rebates, scale, maker-first order
    "test_venue_ledger",              # Fees through the ledger, small exact notionals
    "test_venue_symbol_scale",        # notionalRaw at non-default scales
    "test_venue_engine_clearing",     # rateOnNotional / notionalRaw call sites
    "test_venue_conservation_fuzz",   # fee schedule under randomized flow, conservation
]
FUNDING_REGRESSION = [
    "test_venue_cross_margin",     # CrossMarginManager::applyFunding, its own copy
    "test_venue_engine_clearing",  # Clearing::applyFunding
    "test_venue_perp",             # perp funding end to end
    "test_venue_conservation_fuzz",
]
RENDER_REGRESSION = [
    "test_venue_control_plane",   # ControlApi::instrumentJson via the control server
    "test_venue_control_methods",  # ControlApi method surface
    "test_venue_metrics",         # prom::render, pins "fme_funding_rate 0.00010000"
    "test_venue_gauge_sampler",   # Gauges::fundingRateRaw sampled from live state
    "test_venue_perp_venue",      # Gauges::fundingRateRaw via fundingRateRawOf
]

FEE_TARGETS = sorted({FEE, GOLDEN, *FEE_REGRESSION})
FUNDING_TARGETS = sorted({FUNDING, GOLDEN, *FUNDING_REGRESSION})
RENDER_TARGETS = sorted({RENDER, GOLDEN, *RENDER_REGRESSION})
# ledger.h's rateOnNotional/notionalRaw are shared by the fee and funding
# paths, so a mutation there has to answer to both.
SHARED_TARGETS = sorted(set(FEE_TARGETS) | set(FUNDING_TARGETS))


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
    targets: list[str] = field(default_factory=list)
    occurrence: int = 1
    expected_occurrences: int = 1
    # If set, a GREEN verdict on this mutation does not fail the run: the
    # reason states why no observable assertion could ever distinguish the
    # mutated code from the original, given what the target binaries feed it.
    equivalent_reason: str | None = None

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
    # ======================================================================
    # fees.h -- finding 18
    # ======================================================================
    Mutation(
        name="feerate-truncate",
        why="feeRateRawOf truncates the scaled bps instead of rounding to nearest -- the same "
            "class of error the fix's own docstring describes for funding, applied to the fee "
            "ladder instead",
        file=FEES_H,
        old="  return roundDoubleToI64(bps * (static_cast<double>(kFeeRateScale) / 10'000.0));",
        new="  return static_cast<int64_t>(bps * (static_cast<double>(kFeeRateScale) / 10'000.0));",
        targets=FEE_TARGETS,
    ),
    Mutation(
        name="fee-two-tier-lookups",
        why="the maker and taker rates go back to two separate currentBps() calls instead of "
            "the one lookup the fix merged them into -- the exact shape the fix's own comment "
            "says it removed so the two sides of a print cannot land in different tiers",
        equivalent_reason="FeeSchedule::currentBps mutates only through evictExpired(nowNs), "
                          "which has nothing left to evict on a second call with the same "
                          "timestamp, and resolveTierIndex is a pure read; with a bound "
                          "Account there is no eviction at all. Two lookups at one "
                          "timestamp return the same pair by construction, so no test can "
                          "distinguish them; the idempotence they rely on is pinned instead",
        file=FEES_H,
        old="""  std::pair<int64_t, int64_t> ratesAt(int64_t nowRaw)
  {
    const auto [makerBps, takerBps] = schedule_.currentBps(nowRaw);
    return {feeRateRawOf(makerBps), feeRateRawOf(takerBps)};
  }""",
        new="""  std::pair<int64_t, int64_t> ratesAt(int64_t nowRaw)
  {
    const auto [makerBps, ignoredTaker] = schedule_.currentBps(nowRaw);
    const auto [ignoredMaker, takerBps] = schedule_.currentBps(nowRaw);
    (void)ignoredTaker;
    (void)ignoredMaker;
    return {feeRateRawOf(makerBps), feeRateRawOf(takerBps)};
  }""",
        targets=FEE_TARGETS,
    ),
    Mutation(
        name="fee-computed-in-double-again",
        why="emit() and settle() go back to notional-as-double through FeeSchedule::feeFor -- "
            "the original bug the whole finding is about, reintroduced whole",
        edits=[
            Edit(
                file=FEES_H,
                old="""  void emit(const Trade& t, const SymbolConfig& cfg, int64_t nowRaw, const EventSink& sink)
  {
    if (!enabled_)
    {
      return;
    }
    const Amount notional = notionalOf(t, cfg);
    const auto [makerRateRaw, takerRateRaw] = ratesAt(nowRaw);
    sink(FeeCharged{t.makerId, cfg.id, Volume::fromRaw(feeRawOf(notional, makerRateRaw)), true,
                    t.makerAccount});
    sink(FeeCharged{t.takerId, cfg.id, Volume::fromRaw(feeRawOf(notional, takerRateRaw)), false,
                    t.takerAccount});
  }""",
                new="""  void emit(const Trade& t, const SymbolConfig& cfg, int64_t nowRaw, const EventSink& sink)
  {
    if (!enabled_)
    {
      return;
    }
    const double notionalD = static_cast<double>(notionalOf(t, cfg)) / kMoneyScale;
    sink(FeeCharged{t.makerId, cfg.id, Volume::fromDouble(schedule_.feeFor(nowRaw, notionalD, true)),
                    true, t.makerAccount});
    sink(FeeCharged{t.takerId, cfg.id,
                    Volume::fromDouble(schedule_.feeFor(nowRaw, notionalD, false)), false,
                    t.takerAccount});
  }""",
            ),
            Edit(
                file=FEES_H,
                old="""  void settle(const Trade& t, const SymbolConfig& cfg, int64_t nowRaw, Ledger& ledger,
              uint64_t venueAccount, const EventSink& sink)
  {
    if (!enabled_)
    {
      return;
    }
    const Amount notional = notionalOf(t, cfg);
    const auto [makerRateRaw, takerRateRaw] = ratesAt(nowRaw);
    charge(t.makerId, t.makerAccount, feeRawOf(notional, makerRateRaw), true, cfg, ledger,
           venueAccount, sink);
    charge(t.takerId, t.takerAccount, feeRawOf(notional, takerRateRaw), false, cfg, ledger,
           venueAccount, sink);
  }""",
                new="""  void settle(const Trade& t, const SymbolConfig& cfg, int64_t nowRaw, Ledger& ledger,
              uint64_t venueAccount, const EventSink& sink)
  {
    if (!enabled_)
    {
      return;
    }
    const double notionalD = static_cast<double>(notionalOf(t, cfg)) / kMoneyScale;
    charge(t.makerId, t.makerAccount,
           static_cast<int64_t>(Volume::fromDouble(schedule_.feeFor(nowRaw, notionalD, true)).raw()),
           true, cfg, ledger, venueAccount, sink);
    charge(t.takerId, t.takerAccount,
           static_cast<int64_t>(Volume::fromDouble(schedule_.feeFor(nowRaw, notionalD, false)).raw()),
           false, cfg, ledger, venueAccount, sink);
  }""",
            ),
        ],
        targets=FEE_TARGETS,
    ),
    Mutation(
        name="feerate-round-half-to-even",
        why="feeRateRawOf rounds exact ties to even (banker's rounding) instead of away from "
            "zero -- a different, defensible rounding rule the acceptance corpus cannot "
            "distinguish from the fix because every bps in it (2.5, 7.5, 1.0, 5.0, -1.0, -2.0) "
            "scales to an exact integer raw with no .5 remainder to break either way",
        file=FEES_H,
        old="  return roundDoubleToI64(bps * (static_cast<double>(kFeeRateScale) / 10'000.0));",
        new="""  const double scaled = bps * (static_cast<double>(kFeeRateScale) / 10'000.0);
  const int64_t truncated = static_cast<int64_t>(scaled);
  const double frac = scaled - static_cast<double>(truncated);
  if (frac > 0.5 || (frac == 0.5 && (truncated & 1) != 0))
  {
    return truncated + 1;
  }
  return truncated;""",
        targets=FEE_TARGETS,
    ),
    Mutation(
        name="feerate-clamped-at-100pct",
        why="feeRateRawOf silently clamps bps to 10'000 (100%) before scaling -- invisible to "
            "every bps in the acceptance and regression corpora, which top out at 7.5bps, and "
            "only wrong for a schedule someone configures above 1.0",
        file=FEES_H,
        old="  return roundDoubleToI64(bps * (static_cast<double>(kFeeRateScale) / 10'000.0));",
        new="""  const double clamped = bps > 10'000.0 ? 10'000.0 : bps;
  return roundDoubleToI64(clamped * (static_cast<double>(kFeeRateScale) / 10'000.0));""",
        targets=FEE_TARGETS,
    ),
    Mutation(
        name="fee-taker-scale-mismatched-10x",
        why="the taker rate alone is scaled by kFeeRateScale*10 instead of kFeeRateScale, so the "
            "taker fee comes out a decimal order of magnitude wrong while the maker fee stays "
            "correct",
        file=FEES_H,
        old="""  std::pair<int64_t, int64_t> ratesAt(int64_t nowRaw)
  {
    const auto [makerBps, takerBps] = schedule_.currentBps(nowRaw);
    return {feeRateRawOf(makerBps), feeRateRawOf(takerBps)};
  }""",
        new="""  std::pair<int64_t, int64_t> ratesAt(int64_t nowRaw)
  {
    const auto [makerBps, takerBps] = schedule_.currentBps(nowRaw);
    const int64_t takerRateRaw =
        roundDoubleToI64(takerBps * (static_cast<double>(kFeeRateScale * 10) / 10'000.0));
    return {feeRateRawOf(makerBps), takerRateRaw};
  }""",
        targets=FEE_TARGETS,
    ),
    # ======================================================================
    # ledger.h -- rateOnNotional / notionalRaw, shared by fees and funding
    # ======================================================================
    Mutation(
        name="rate-on-notional-rounds",
        why="rateOnNotional rounds to nearest instead of truncating toward zero the way "
            "notionalRaw (and the rest of the venue's money arithmetic) does",
        file=LEDGER_H,
        old="""  if (notional <= kI64Max && notional >= kI64Min)
  {
    return static_cast<Amount>(mulDivI64(static_cast<int64_t>(notional), rateRaw, rateScale));
  }
  return notional * rateRaw / rateScale;""",
        new="""  if (notional <= kI64Max && notional >= kI64Min)
  {
    const __int128 num = static_cast<__int128>(static_cast<int64_t>(notional)) * rateRaw;
    const __int128 half = rateScale / 2;
    const __int128 rounded = (num >= 0) ? (num + half) / rateScale : (num - half) / rateScale;
    return static_cast<Amount>(rounded);
  }
  return notional * rateRaw / rateScale;""",
        targets=SHARED_TARGETS,
    ),
    Mutation(
        name="wide-notional-clamped",
        why="a notional wider than int64 is clamped into int64 range instead of taking the "
            "128-bit multiply-divide directly, the exact regression the fix's own comment says "
            "it refused to make",
        file=LEDGER_H,
        old="""  if (notional <= kI64Max && notional >= kI64Min)
  {
    return static_cast<Amount>(mulDivI64(static_cast<int64_t>(notional), rateRaw, rateScale));
  }
  return notional * rateRaw / rateScale;""",
        new="""  const Amount clamped = notional > kI64Max ? kI64Max : (notional < kI64Min ? kI64Min : notional);
  return static_cast<Amount>(mulDivI64(static_cast<int64_t>(clamped), rateRaw, rateScale));""",
        targets=SHARED_TARGETS,
    ),
    # ======================================================================
    # messages.h -- finding 19, the one conversion
    # ======================================================================
    Mutation(
        name="fundingrate-truncate",
        why="fundingRateRawOf truncates instead of rounding to nearest -- the finding's own "
            "headline number, 0.0003 -> 29999 instead of 30000",
        file=MESSAGES_H,
        old="  return roundDoubleToI64(rate * static_cast<double>(kFundingRateScale));",
        new="  return static_cast<int64_t>(rate * static_cast<double>(kFundingRateScale));",
        targets=FUNDING_TARGETS,
    ),
    # ======================================================================
    # engine/clearing.h -- finding 19, the settlement copy
    # ======================================================================
    Mutation(
        name="clearing-double-again",
        why="the per-position payment goes back to a double product of the notional and the "
            "raw double rate -- the settlement half of the original bug, with the published "
            "rate itself (fundingRateRaw_) left correct so only the money moved is wrong",
        file=CLEARING_H,
        old="""      const Amount notional =
          notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
      const Amount mag = rateOnNotional(notional, rateRaw, kFundingRateScale);""",
        new="""      const Amount notional =
          notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
      const Amount mag = static_cast<Amount>(static_cast<double>(notional) * rate);""",
        targets=FUNDING_TARGETS,
    ),
    Mutation(
        name="clearing-conversion-done-twice",
        why="the raw rate is converted back to a double and re-scaled before the payment, a "
            "second double round trip after the one boundary conversion the fix insists on "
            "doing exactly once",
        file=CLEARING_H,
        old="      const Amount mag = rateOnNotional(notional, rateRaw, kFundingRateScale);",
        new="""      const double rateBack = static_cast<double>(rateRaw) / static_cast<double>(kFundingRateScale);
      const int64_t rateRawAgain =
          roundDoubleToI64(rateBack * static_cast<double>(kFundingRateScale));
      const Amount mag = rateOnNotional(notional, rateRawAgain, kFundingRateScale);""",
        targets=FUNDING_TARGETS,
    ),
    # ======================================================================
    # cross_margin.h -- finding 19, the portfolio-book copy
    # ======================================================================
    Mutation(
        name="cross-margin-double-again",
        why="CrossMarginManager's own copy of the funding expression goes back to the double "
            "product -- the fix's note that this call site carries the same bug as clearing.h, "
            "undone",
        file=CROSS_MARGIN_H,
        old="      const Amount pay = -rateOnNotional(notionalSigned, rateRaw, kFundingRateScale);",
        new="      const Amount pay = -static_cast<Amount>(static_cast<double>(notionalSigned) * rate);",
        targets=FUNDING_TARGETS,
    ),
    Mutation(
        name="clearing-accessor-gauge-double-again",
        why="the published-rate accessor Clearing::fundingRateRaw() -- what a deployment reads "
            "to sample the observability gauge -- round-trips the raw through a double instead "
            "of returning fundingRateRaw_ directly, reintroducing the funding finding at the "
            "gauge-sampling seam specifically",
        file=CLEARING_H,
        old="  int64_t fundingRateRaw() const noexcept { return fundingRateRaw_; }",
        new="""  int64_t fundingRateRaw() const noexcept
  {
    const double asDouble =
        static_cast<double>(fundingRateRaw_) / static_cast<double>(kFundingRateScale);
    return static_cast<int64_t>(asDouble * static_cast<double>(kFundingRateScale));
  }""",
        targets=FUNDING_TARGETS,
    ),
    # ======================================================================
    # fixed_point_text.h -- finding 20
    # ======================================================================
    Mutation(
        name="render-sign-dropped",
        why="fixedPointToStr never writes the sign at all -- every negative raw renders as its "
            "positive magnitude",
        file=FIXED_POINT_TEXT_H,
        old="""  if (negative)
  {
    *--p = '-';
  }
  return std::string(p, end);""",
        new="  return std::string(p, end);",
        targets=RENDER_TARGETS,
    ),
    Mutation(
        name="render-sign-dropped-when-integral-part-zero",
        why="the sign is written only when the integral part is nonzero, so a negative raw "
            "whose magnitude is under 1.0 -- -0.0003 rendered as 0.00030000 instead of "
            "-0.00030000 -- loses its sign; the fundamental value is still wrong even though "
            "the general (nonzero-integral) case works",
        file=FIXED_POINT_TEXT_H,
        old="""  if (negative)
  {
    *--p = '-';
  }
  return std::string(p, end);""",
        new="""  if (negative && mag / divisor > 0)
  {
    *--p = '-';
  }
  return std::string(p, end);""",
        targets=RENDER_TARGETS,
    ),
    Mutation(
        name="render-drops-leading-fraction-zeros",
        why="the fractional digit loop stops as soon as the remaining fraction hits zero "
            "instead of always emitting every decimal the scale has, so 1 raw at 1e8 prints "
            "\"0.1\" instead of \"0.00000001\" -- the exact defect the fix's docstring names "
            "(a one-tick instrument answering 0.000000)",
        file=FIXED_POINT_TEXT_H,
        old="""  for (int i = 0; i < digits; ++i)
  {
    *--p = static_cast<char>('0' + static_cast<int>(frac % 10));
    frac /= 10;
  }""",
        new="""  for (int i = 0; i < digits && (frac != 0 || i == 0); ++i)
  {
    *--p = static_cast<char>('0' + static_cast<int>(frac % 10));
    frac /= 10;
  }""",
        targets=RENDER_TARGETS,
    ),
    # ======================================================================
    # control_api.h -- finding 20, the control-plane surface
    # ======================================================================
    Mutation(
        name="control-api-tick-uses-qty-scale",
        why="the instrument's tick is rendered at the symbol's QUANTITY scale instead of its "
            "PRICE scale -- a raw meant to be read against one scale is spelled out against "
            "the other",
        file=CONTROL_API_H,
        old="""    return std::string("{\\"ok\\":true,\\"symbol\\":") + std::to_string(c.id) +
           ",\\"tick\\":" + fixedPointToStr(c.tickSize.raw(), c.priceScale) +""",
        new="""    return std::string("{\\"ok\\":true,\\"symbol\\":") + std::to_string(c.id) +
           ",\\"tick\\":" + fixedPointToStr(c.tickSize.raw(), c.qtyScale) +""",
        targets=RENDER_TARGETS,
    ),
    Mutation(
        name="control-api-minprice-tostring",
        why="minPrice alone goes back through std::to_string(double) -- six decimals, C-locale "
            "-- while tick and maxPrice stay on the fixed-point renderer",
        file=CONTROL_API_H,
        old="""           ",\\"minPrice\\":" + fixedPointToStr(c.minPrice.raw(), c.priceScale) +""",
        new="""           ",\\"minPrice\\":" + std::to_string(c.minPrice.toDouble()) +""",
        targets=RENDER_TARGETS,
    ),
    # ======================================================================
    # prometheus.h -- finding 20, the metrics surface
    # ======================================================================
    Mutation(
        name="prometheus-funding-rate-six-digits",
        why="fme_funding_rate goes back through std::to_string(double) -- six decimals, "
            "C-locale -- undoing the one line the fix changed in this file",
        file=PROMETHEUS_H,
        old="""  gaugeStr(out, "fme_funding_rate", "Last settled funding rate (fraction)",
           fixedPointToStr(g.fundingRateRaw, kFundingRateScale));""",
        new="""  gaugeStr(out, "fme_funding_rate", "Last settled funding rate (fraction)",
           std::to_string(static_cast<double>(g.fundingRateRaw) / static_cast<double>(kFundingRateScale)));""",
        targets=RENDER_TARGETS,
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


def object_files(target: str) -> list[Path]:
    """The target's own object files, located the way `find` would."""
    out = subprocess.run(
        ["find", str(BUILD), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, target: str, output: str):
        super().__init__(f"rebuild of {target} failed")
        self.target = target
        self.output = output


def rebuild(target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(target, output)
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def rebuild_all() -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed("(all)", output)
    return output


def run_test(target: str) -> tuple[int, str]:
    binary = BIN_DIR / target
    try:
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def summary_line(output: str) -> str:
    return next((line for line in output.splitlines()
                if line.startswith("[==========] ") and " ran." in line), "")


def all_targets(selected: list[Mutation]) -> list[str]:
    return sorted({t for m in selected for t in m.targets})


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        for obj in object_files(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target)
        state = "green" if code == 0 else "RED"
        print(f"  control {target:<34} {state}   {summary_line(output).strip()}")
        ok = ok and code == 0
    return ok


def run_ctest() -> tuple[bool, str]:
    result = subprocess.run(
        ["ctest", "--output-on-failure", "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=CTEST_TIMEOUT, cwd=BUILD,
    )
    return result.returncode == 0, result.stdout + result.stderr


def failed_tests(ctest_output: str) -> list[str]:
    return [line.strip() for line in ctest_output.splitlines() if "***Failed" in line
            or "Failed" in line and line.strip().startswith("Test #")]


def run_mutation(m: Mutation, skip_full_suite: bool) -> str:
    """'killed', 'killed-by-full-suite', 'alive' or 'no-compile'."""
    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    if m.equivalent_reason:
        print(f"  equivalence claim: {m.equivalent_reason}")
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
        for target in m.targets:
            removed = object_files(target)
            for obj in removed:
                obj.unlink()
            print(f"  removed {len(removed)} object file(s) for {target}")

        try:
            for target in m.targets:
                output = rebuild(target)
                compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
                print(f"  rebuilt {target}: {compiled} 'Building CXX' line(s)")
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-20:]))
            return "no-compile"

        for target in m.targets:
            code, test_output = run_test(target)
            red = code != 0
            print(f"  {target} -> exit {code} ({'RED, mutation killed' if red else 'green'})")
            if red:
                verdict = "killed"
                failed = [line for line in test_output.splitlines()
                          if line.startswith("[  FAILED  ]")]
                for line in failed[:6]:
                    print(f"    {line}")

        if verdict == "alive" and not skip_full_suite:
            print("  every curated target green -- escalating to the full venue-lite suite "
                  "before calling this a survivor")
            try:
                build_output = rebuild_all()
                compiled = sum(1 for line in build_output.splitlines() if "Building CXX" in line)
                print(f"  rebuilt (all): {compiled} 'Building CXX' line(s)")
            except BuildFailed as e:
                print("  full-suite rebuild DID NOT COMPILE -- not a mutation")
                print("\n".join(e.output.splitlines()[-20:]))
                return "no-compile"
            ok, ctest_output = run_ctest()
            if ok:
                print("  full suite: 100% tests passed")
            else:
                verdict = "killed-by-full-suite"
                print("  full suite: FAILURES -- not actually a survivor")
                for line in failed_tests(ctest_output)[:20]:
                    print(f"    {line}")

        if verdict == "alive":
            scope = "every target asked, including the full suite" if not skip_full_suite \
                else "every curated target asked (full-suite escalation skipped)"
            if m.equivalent_reason:
                print(f"  GREEN, MUTATION SURVIVED {scope} -- claimed equivalent (see reason above)")
            else:
                print(f"  GREEN, MUTATION SURVIVED {scope}")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for target in m.targets:
            for obj in object_files(target):
                obj.unlink()

    return verdict


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--skip-full-suite", action="store_true",
                        help="do not escalate survivors to the full ctest suite")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            eq = "  [equivalence claimed]" if m.equivalent_reason else ""
            print(f"{m.name:<42} {', '.join(m.files())}{eq}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD} is not configured; run\n  cmake --preset venue-lite")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = all_targets(selected)

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m, args.skip_full_suite)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    label = {"killed": "RED  ", "killed-by-full-suite": "RED* ", "alive": "ALIVE",
             "no-compile": "NOBLD"}
    for m, verdict in results:
        eq = "  (claimed equivalent)" if (verdict == "alive" and m.equivalent_reason) else ""
        print(f"  {label[verdict]}  {m.name:<42} {', '.join(m.files())}{eq}")

    survived = [m.name for m, v in results if v == "alive" and not m.equivalent_reason]
    equivalent = [m.name for m, v in results if v == "alive" and m.equivalent_reason]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    fullsuite = [m.name for m, v in results if v == "killed-by-full-suite"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if equivalent:
        print(f"{len(equivalent)} mutation(s) survived but are claimed equivalent: "
              f"{', '.join(equivalent)}")
    if fullsuite:
        print(f"{len(fullsuite)} mutation(s) were killed only by the full suite, not by any "
              f"curated target: {', '.join(fullsuite)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
