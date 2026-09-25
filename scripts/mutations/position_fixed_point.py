#!/usr/bin/env python3
"""Mutation harness for the position/clearing fixed-point fix.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the fix one piece at a time,
in the source, and checks that the acceptance tests -- and the regression
suites the fix's own author listed as touched -- go red. A mutation that
neither binary catches is a hole in the tests, reported as a survivor.

Three acceptance binaries answer for the three findings the fix covers:

    test_nlevel_tick_rounding          tick snapping (book)
    test_position_tracker_fixed_point  cost basis / realised PnL (position)
    test_clearing_fixed_point          equity / rolling window (clearing)

Each mutation also runs against whichever regression suites its area's call
sites reach, per the task note's own account of what it touched:

    test_account, test_liquidation_engine, test_cross_cascade,
    test_venue_stack, test_w15_composition, test_w15_realism_integration,
    test_w15_reproducibility, test_w15_calibration

A mutation is killed if ANY target binary goes red. Where a mutation maps to
one or two specific acceptance tests, that narrow gtest_filter runs first
(fast, targeted); if it stays green the harness falls back to the whole
binary, in case some other test in the same file happens to catch the
change. Regression targets have no narrow filter -- they always run whole,
because what matters there is that nothing else broke, not one named test.

Every run is honest about the build: the mutated file's hash is printed
before and after, every target's object files are deleted so nothing can be
served from cache, the rebuild output has to contain "Building CXX" or the
run is refused, and each binary runs under a timeout. A mutation that does
not compile is not a mutation and is reported as such.

A mutation may carry `equivalent_reason`: a mutation that provably cannot
change any observable value under the arithmetic actually exercised (e.g.
swapping a saturating add for a plain one where nothing overflows) is still
run and still reported, but does not fail the overall exit code -- the
reason is printed next to the verdict either way.

Usage:

    python3 scripts/mutations/position_fixed_point.py            # everything
    python3 scripts/mutations/position_fixed_point.py --list
    python3 scripts/mutations/position_fixed_point.py --only closepos-wrong-sign

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON -DFLOX_ENABLE_BACKTEST=ON
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
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 120

BOOK = "include/flox/book/nlevel_order_book.h"
TRACKER = "include/flox/position/position_tracker.h"
ACCOUNT_H = "include/flox/clearing/account.h"
ACCOUNT_CPP = "src/clearing/account.cpp"
MATH = "include/flox/util/base/math.h"

NLEVEL = "test_nlevel_tick_rounding"
TRACKER_T = "test_position_tracker_fixed_point"
CLEARING = "test_clearing_fixed_point"

# Regression suites the code agent's task note lists as touched by the fix's
# call-site changes. These have no narrow filter -- the whole binary is the
# check.
CLEARING_REGRESSION = [
    "test_account",
    "test_liquidation_engine",
    "test_cross_cascade",
    "test_venue_stack",
    "test_w15_composition",
    "test_w15_realism_integration",
    "test_w15_reproducibility",
    "test_w15_calibration",
]

NLEVEL_TARGETS = [NLEVEL]
TRACKER_TARGETS = [TRACKER_T]
CLEARING_TARGETS = [CLEARING] + CLEARING_REGRESSION


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
    # Narrow gtest_filter, applied ONLY to filter_target. Every other target
    # in `targets` (and filter_target itself if the narrow run stays green)
    # runs the whole binary.
    gtest_filter: str | None = None
    filter_target: str | None = None
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
    # nlevel_order_book.h -- tick snapping (finding 14)
    # ======================================================================
    Mutation(
        name="tick-snap-sides-swapped",
        why="ticks() hands sdiv_floor to asks and sdiv_ceil to bids -- the exact reverse of "
            "the fix -- so an off-tick ask snaps a tick BELOW the quote and a bid snaps a "
            "tick ABOVE it, inventing liquidity on both sides",
        file=BOOK,
        old="""    return (side == Side::SELL) ? math::sdiv_ceil(pr, _tickSizeDiv)
                                : math::sdiv_floor(pr, _tickSizeDiv);""",
        new="""    return (side == Side::SELL) ? math::sdiv_floor(pr, _tickSizeDiv)
                                : math::sdiv_ceil(pr, _tickSizeDiv);""",
        targets=NLEVEL_TARGETS,
        gtest_filter="TickRoundingTest.AskBetweenTicksIsNeverStoredBelowTheQuote:"
                     "TickRoundingTest.BidBetweenTicksIsNeverStoredAboveTheQuote:"
                     "TickRoundingTest.SubTickQuotesOnACentTickStayConservative:"
                     "TickRoundingTest.EveryOffsetInsideATickSnapsAwayFromTheQuote",
        filter_target=NLEVEL,
    ),
    Mutation(
        name="snapshot-acc-snaps-the-other-way",
        why="the snapshot's acc() scan -- which only sizes the reanchor window -- is fed the "
            "opposite side from the write loop right below it: bids scanned as asks and back. "
            "Every acceptance quote sits nowhere near the edge of a 512-level window, so the "
            "wrong-side range never actually excludes the tick the write loop places",
        file=BOOK,
        old="""      acc(up.bids, Side::BUY);
      acc(up.asks, Side::SELL);""",
        new="""      acc(up.bids, Side::SELL);
      acc(up.asks, Side::BUY);""",
        targets=NLEVEL_TARGETS,
    ),
    Mutation(
        name="bid-ask-at-price-snap-nearest-again",
        why="bidAtPrice/askAtPrice go back to unconditional nearest-tick rounding instead of "
            "the side-aware snap applyBookUpdate uses to place the same price -- no acceptance "
            "test, and no regression suite in this area, ever calls either query",
        file=BOOK,
        old="""  inline Quantity bidAtPrice(Price p) const override
  {
    const size_t i = localIndex(p, Side::BUY);
    return i < MAX_LEVELS ? _bids[i] : Quantity{};
  }

  inline Quantity askAtPrice(Price p) const override
  {
    const size_t i = localIndex(p, Side::SELL);
    return i < MAX_LEVELS ? _asks[i] : Quantity{};
  }""",
        new="""  inline Quantity bidAtPrice(Price p) const override
  {
    const int64_t t = math::sdiv_round_nearest(p.raw(), _tickSizeDiv) - _baseIndex;
    const size_t i = (static_cast<uint64_t>(t) < static_cast<uint64_t>(MAX_LEVELS))
                          ? static_cast<size_t>(t)
                          : MAX_LEVELS;
    return i < MAX_LEVELS ? _bids[i] : Quantity{};
  }

  inline Quantity askAtPrice(Price p) const override
  {
    const int64_t t = math::sdiv_round_nearest(p.raw(), _tickSizeDiv) - _baseIndex;
    const size_t i = (static_cast<uint64_t>(t) < static_cast<uint64_t>(MAX_LEVELS))
                          ? static_cast<size_t>(t)
                          : MAX_LEVELS;
    return i < MAX_LEVELS ? _asks[i] : Quantity{};
  }""",
        targets=NLEVEL_TARGETS,
    ),
    Mutation(
        name="sdiv-floor-truncates-toward-zero",
        why="own mutation: sdiv_floor's negative branch drops the correction that turns a "
            "truncating division into a floor, so a quote below zero snaps toward zero "
            "instead of away from it -- a bid stored above what was quoted. Only a negative "
            "price reaches that branch (ticks() is handed the price, not an offset from the "
            "window anchor), and no test quoted one",
        file=MATH,
        old="  return exact ? -(int64_t)q : -(int64_t)q - 1;",
        new="  return -(int64_t)q;",
        targets=NLEVEL_TARGETS,
        gtest_filter="TickRoundingTest.NegativeQuotesSnapAwayFromTheQuote:"
                     "TickRoundingTest.EveryOffsetInsideATickSnapsAwayFromTheQuoteBelowZero",
        filter_target=NLEVEL,
    ),
    Mutation(
        name="delta-reanchor-scan-sides-swapped",
        why="own mutation: reanchorForDelta's scan() -- the delta-feed counterpart of acc() -- "
            "is fed the opposite side too. No acceptance test or listed regression suite ever "
            "sends a DELTA update through this book, so the whole delta path is unreached",
        file=BOOK,
        old="""    scan(up.bids, Side::BUY);
    scan(up.asks, Side::SELL);""",
        new="""    scan(up.bids, Side::SELL);
    scan(up.asks, Side::BUY);""",
        targets=NLEVEL_TARGETS,
    ),

    # ======================================================================
    # position_tracker.h -- cost basis / realised PnL (finding 19)
    # ======================================================================
    Mutation(
        name="weighted-sum-drops-remainder",
        why="average() returns floor(_units * Scale / _weight) alone, discarding the tail "
            "built from _fraction: exactly the per-lot rounding the type exists to remove",
        file=TRACKER,
        old="""    const uint64_t tail = (static_cast<uint64_t>(leftover) + static_cast<uint64_t>(_fraction)) /
                          static_cast<uint64_t>(_weight);
    return Price::fromRaw(checkedAddI64(whole, static_cast<int64_t>(tail)));""",
        new="""    return Price::fromRaw(whole);""",
        targets=TRACKER_TARGETS,
        gtest_filter="PositionTrackerFixedPoint.AvgEntryPriceIsExactAfterAThousandIdenticalFills",
        filter_target=TRACKER_T,
    ),
    Mutation(
        name="weighted-sum-final-division-rounds-up",
        why="the one unavoidable rounding step -- average()'s final division -- rounds the "
            "tail up instead of truncating. Every acceptance fill sequence is built from a "
            "single repeated price, so the true remainder is always exactly zero and ceil(0/w) "
            "equals floor(0/w): the direction of that division is never actually exercised",
        file=TRACKER,
        old="""    const uint64_t tail = (static_cast<uint64_t>(leftover) + static_cast<uint64_t>(_fraction)) /
                          static_cast<uint64_t>(_weight);
    return Price::fromRaw(checkedAddI64(whole, static_cast<int64_t>(tail)));""",
        new="""    const uint64_t num = static_cast<uint64_t>(leftover) + static_cast<uint64_t>(_fraction);
    const uint64_t w = static_cast<uint64_t>(_weight);
    const uint64_t tail = (num == 0) ? 0 : (num + w - 1) / w;
    return Price::fromRaw(checkedAddI64(whole, static_cast<int64_t>(tail)));""",
        targets=TRACKER_TARGETS,
    ),
    Mutation(
        name="average-basis-from-previous-rounded-average",
        why="addLot's AVERAGE branch goes back to weighting the OLD (already-rounded) lot "
            "price by absolute quantity in double, instead of re-deriving the basis from the "
            "two exact weighted sums -- the rounding this file exists to stop from compounding",
        file=TRACKER,
        old="""        detail::WeightedPriceSum sum;
        sum.add(s.lots.front().quantity, s.lots.front().price);
        sum.add(signedQty, price);
        s.lots.front().quantity = Quantity::fromRaw(newQty);
        s.lots.front().price = sum.average();""",
        new="""        double absOld = std::abs(s.lots.front().quantity.toDouble());
        double absNew = std::abs(signedQty.toDouble());
        double absTotal = std::abs(Quantity::fromRaw(newQty).toDouble());
        double notional = absOld * s.lots.front().price.toDouble() +
                          absNew * price.toDouble();
        s.lots.front().quantity = Quantity::fromRaw(newQty);
        s.lots.front().price = Price::fromDouble(notional / absTotal);""",
        targets=TRACKER_TARGETS,
        gtest_filter="PositionTrackerFixedPoint.AverageCostBasisStaysExactAcrossAThousandFills:"
                     "PositionTrackerFixedPoint.AverageModeRealizedPnlIsExact",
        filter_target=TRACKER_T,
    ),
    Mutation(
        name="fifo-realisation-via-double",
        why="closePosition's per-lot realisation goes back to double: price difference and "
            "product computed as double and converted back through Price::fromDouble, the "
            "exact path the fix replaced with mulDivI64 on the raw",
        file=TRACKER,
        old="""      const int64_t priceDiff = checkedSubI64(closePrice.raw(), lot.price.raw());
      int64_t lotPnl = mulDivI64(priceDiff, closeQty, Volume::Scale);
      if (!wasLong)
      {
        lotPnl = -lotPnl;
      }
      pnl = checkedAddI64(pnl, lotPnl);""",
        new="""      double priceDiffD = closePrice.toDouble() - lot.price.toDouble();
      double lotPnlD = priceDiffD * Quantity::fromRaw(closeQty).toDouble();
      if (!wasLong)
      {
        lotPnlD = -lotPnlD;
      }
      pnl += Price::fromDouble(lotPnlD).raw();""",
        targets=TRACKER_TARGETS,
        gtest_filter="PositionTrackerFixedPoint.RealizedPnlIsExactAcrossAThousandLots",
        filter_target=TRACKER_T,
    ),
    Mutation(
        name="realisation-checked-sub-to-plain",
        why="the price-diff subtraction in closePosition loses its overflow guard: "
            "checkedSubI64 becomes a plain int64 '-'. Every price in the suite sits well "
            "inside int64 range for the subtraction, so nothing here can actually overflow",
        file=TRACKER,
        old="const int64_t priceDiff = checkedSubI64(closePrice.raw(), lot.price.raw());",
        new="const int64_t priceDiff = closePrice.raw() - lot.price.raw();",
        targets=TRACKER_TARGETS,
        equivalent_reason="checkedSubI64 only changes the result when the true difference "
                          "overflows int64; every closePrice/lot.price pair in the acceptance "
                          "and regression data is far inside range, so the checked and plain "
                          "subtractions are bit-identical here.",
    ),
    Mutation(
        name="get-realized-pnl-returns-price",
        why="getRealizedPnl/getTotalRealizedPnl go back to returning Price instead of Volume "
            "-- the type the acceptance suite pins with a decltype check specifically so this "
            "regresses at run time rather than needing a second binary",
        file=TRACKER,
        old="""  Volume getRealizedPnl(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].realizedPnl;
  }

  Volume getTotalRealizedPnl() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t total = 0;
    _states.forEach([&total](SymbolId, const PositionState& s)
                    { total = checkedAddI64(total, s.realizedPnl.raw()); });
    return Volume::fromRaw(total);
  }""",
        new="""  Price getRealizedPnl(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return Price::fromRaw(_states[symbol].realizedPnl.raw());
  }

  Price getTotalRealizedPnl() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t total = 0;
    _states.forEach([&total](SymbolId, const PositionState& s)
                    { total = checkedAddI64(total, s.realizedPnl.raw()); });
    return Price::fromRaw(total);
  }""",
        targets=TRACKER_TARGETS,
        gtest_filter="PositionTrackerFixedPoint.RealizedPnlIsMoneyTyped",
        filter_target=TRACKER_T,
    ),
    Mutation(
        name="average-mode-short-sign-not-flipped",
        why="own mutation: the sign flip for a closing short (`if (!wasLong)`) is skipped "
            "specifically under AVERAGE cost basis. The acceptance suite tests AVERAGE only "
            "with a long (buy-then-sell) and tests a short only under FIFO -- the AVERAGE + "
            "short combination is never exercised",
        file=TRACKER,
        old="""      if (!wasLong)
      {
        lotPnl = -lotPnl;
      }""",
        new="""      if (!wasLong && _method != CostBasisMethod::AVERAGE)
      {
        lotPnl = -lotPnl;
      }""",
        targets=TRACKER_TARGETS,
    ),

    # ======================================================================
    # account.h / account.cpp -- equity realisation and rolling window
    # (findings 11 and 20)
    # ======================================================================
    Mutation(
        name="closepos-realises-nothing",
        why="the original bug, restored: closePosition erases the legs and never reads the "
            "mark, so a position marked into profit or loss books nothing to equity",
        file=ACCOUNT_CPP,
        old="""  // Realise first, erase second: the legs are what the PnL is computed from.
  const Price mark = markFor(symbol);
  if (mark.raw() > 0)
  {
    int64_t realised = 0;
    for (const auto& p : _positions)
    {
      if (p.symbol != symbol)
      {
        continue;
      }
      realised = checkedAddI64(realised, legUnrealisedPnlRaw(p, mark));
    }
    _equity = Volume::fromRaw(checkedAddI64(_equity.raw(), realised));
  }

  _positions.erase(""",
        new="""  _positions.erase(""",
        targets=CLEARING_TARGETS,
        gtest_filter="ClearingFixedPoint.ClosePositionRealisesTheMarkedPnlIntoEquity:"
                     "ClearingFixedPoint.ClosePositionRealisesALossIntoEquity:"
                     "ClearingFixedPoint.ClosedLegsAccumulateIntoEquityExactly",
        filter_target=CLEARING,
    ),
    Mutation(
        name="closepos-wrong-sign",
        why="the realised amount is subtracted from equity instead of added, so a winning "
            "close books a loss and a losing close books a gain",
        file=ACCOUNT_CPP,
        old="_equity = Volume::fromRaw(checkedAddI64(_equity.raw(), realised));",
        new="_equity = Volume::fromRaw(checkedSubI64(_equity.raw(), realised));",
        targets=CLEARING_TARGETS,
        gtest_filter="ClearingFixedPoint.ClosePositionRealisesTheMarkedPnlIntoEquity:"
                     "ClearingFixedPoint.ClosePositionRealisesALossIntoEquity:"
                     "ClearingFixedPoint.ClosedLegsAccumulateIntoEquityExactly",
        filter_target=CLEARING,
    ),
    Mutation(
        name="closepos-keeps-the-legs",
        why="the erase-by-symbol call is dropped, so a closed position stays in the book: "
            "equity moves once and then moves again every time closePosition is called on "
            "the same symbol",
        file=ACCOUNT_CPP,
        old="""  _positions.erase(
      std::remove_if(_positions.begin(), _positions.end(),
                     [&](const LeveragedPosition& p)
                     { return p.symbol == symbol; }),
      _positions.end());""",
        new="""  (void)std::remove_if(_positions.begin(), _positions.end(),
                       [&](const LeveragedPosition& p)
                       { return p.symbol == symbol; });""",
        targets=CLEARING_TARGETS,
        gtest_filter="ClearingFixedPoint.ClosePositionRealisesTheMarkedPnlIntoEquity:"
                     "ClearingFixedPoint.ClosedLegsAccumulateIntoEquityExactly",
        filter_target=CLEARING,
    ),
    Mutation(
        name="closepos-ignores-contract-multiplier",
        why="legUnrealisedPnlRaw drops its second mulDivI64 -- the contractMultiplier scale -- "
            "so an option or a future books its PnL as if the multiplier were 1. Every "
            "acceptance test in test_clearing_fixed_point opens at the default multiplier, so "
            "only a regression suite that sets one can catch this",
        file=ACCOUNT_CPP,
        old="""int64_t Account::legUnrealisedPnlRaw(const LeveragedPosition& p, Price mark)
{
  const int64_t diff = checkedSubI64(mark.raw(), p.entryPrice.raw());
  const int64_t pnl = mulDivI64(p.quantity.raw(), diff, Volume::Scale);
  return mulDivI64(pnl, p.contractMultiplier.raw(), Quantity::Scale);
}""",
        new="""int64_t Account::legUnrealisedPnlRaw(const LeveragedPosition& p, Price mark)
{
  const int64_t diff = checkedSubI64(mark.raw(), p.entryPrice.raw());
  return mulDivI64(p.quantity.raw(), diff, Volume::Scale);
}""",
        targets=CLEARING_TARGETS,
    ),
    Mutation(
        name="legnotional-uses-entry-not-mark",
        why="legNotionalRaw is handed the caller's chosen price (mark, or entry when unmarked) "
            "but ignores it and reads p.entryPrice directly, so a marked position's notional "
            "is reported at its entry price forever",
        file=ACCOUNT_CPP,
        old="""int64_t Account::legNotionalRaw(const LeveragedPosition& p, Price mark)
{
  const int64_t absQty = p.quantity.raw() < 0 ? -p.quantity.raw() : p.quantity.raw();
  // The multiplier scales money, so it is applied to the notional rather than
  // to the quantity: a fractional multiplier would otherwise round the
  // position size before it ever reached a price.
  const int64_t notional = mulDivI64(absQty, mark.raw(), Volume::Scale);
  return mulDivI64(notional, p.contractMultiplier.raw(), Quantity::Scale);
}""",
        new="""int64_t Account::legNotionalRaw(const LeveragedPosition& p, Price mark)
{
  (void)mark;
  const int64_t absQty = p.quantity.raw() < 0 ? -p.quantity.raw() : p.quantity.raw();
  const int64_t notional = mulDivI64(absQty, p.entryPrice.raw(), Volume::Scale);
  return mulDivI64(notional, p.contractMultiplier.raw(), Quantity::Scale);
}""",
        targets=CLEARING_TARGETS,
        gtest_filter="ClearingFixedPoint.TotalNotionalIsExactAcrossManyLegs:"
                     "ClearingFixedPoint.ModestAggregatesAreExactToday",
        filter_target=CLEARING,
    ),
    Mutation(
        name="rolling-window-subtracts-recomputed-value",
        why="evictExpired subtracts a value round-tripped through double instead of the exact "
            "raw the fill was recorded with -- the same kind of drift the fixed-point rewrite "
            "exists to remove, reintroduced at the one place a stored raw was already in hand",
        file=ACCOUNT_CPP,
        old="""    _rollingTotal =
        Volume::fromRaw(checkedSubI64(_rollingTotal.raw(), _rolling.front().notional.raw()));""",
        new="""    const Volume recomputed = Volume::fromDouble(_rolling.front().notional.toDouble());
    _rollingTotal = Volume::fromRaw(checkedSubI64(_rollingTotal.raw(), recomputed.raw()));""",
        targets=CLEARING_TARGETS,
        gtest_filter="ClearingFixedPoint.RollingNotionalKeepsSmallFillsBesideALargeOne:"
                     "ClearingFixedPoint.RollingNotionalSumsFillsExactly",
        filter_target=CLEARING,
    ),
    Mutation(
        name="rolling-total-clamp-reinstated",
        why="the zero-clamp the fix removed from evictExpired is put back. Under exact "
            "fixed-point bookkeeping the running total can only reach a value that was really "
            "accumulated from stored fills, so it structurally cannot go negative unless a "
            "caller records a negative notional -- which nothing in the suite does",
        file=ACCOUNT_CPP,
        old="""    _rollingTotal =
        Volume::fromRaw(checkedSubI64(_rollingTotal.raw(), _rolling.front().notional.raw()));
    _rolling.pop_front();
  }
  // No clamp: what goes in comes back out to the raw, so a total below zero
  // would mean a fill was recorded negative, not that the sum drifted, and
  // rewriting it as zero would throw away every fill still in the window.
}""",
        new="""    _rollingTotal =
        Volume::fromRaw(checkedSubI64(_rollingTotal.raw(), _rolling.front().notional.raw()));
    _rolling.pop_front();
  }
  if (_rollingTotal.raw() < 0)
  {
    _rollingTotal = Volume{};
  }
}""",
        targets=CLEARING_TARGETS,
        gtest_filter="ClearingFixedPoint.RollingNotionalKeepsSmallFillsBesideALargeOne",
        filter_target=CLEARING,
    ),
    Mutation(
        name="closepos-realises-only-first-leg",
        why="own mutation: the realisation loop over legs on the closed symbol breaks after "
            "the first match, so a second leg on the same symbol (hedge-mode style) is erased "
            "with its PnL never reaching equity. test_clearing_fixed_point never opens two legs "
            "on one symbol before closing; test_account does",
        file=ACCOUNT_CPP,
        old="""      realised = checkedAddI64(realised, legUnrealisedPnlRaw(p, mark));
    }
    _equity = Volume::fromRaw(checkedAddI64(_equity.raw(), realised));""",
        new="""      realised = checkedAddI64(realised, legUnrealisedPnlRaw(p, mark));
      break;
    }
    _equity = Volume::fromRaw(checkedAddI64(_equity.raw(), realised));""",
        targets=CLEARING_TARGETS,
    ),
    Mutation(
        name="rolling-by-symbol-via-double",
        why="own mutation: rollingNotionalBySymbol30d's per-symbol accumulation goes back to "
            "double (fromDouble(total.toDouble() + fill.toDouble())) instead of the exact "
            "checkedAddI64 the fix uses everywhere else. No acceptance test calls this "
            "function at all, and the regression suite's own checks use round dollar figures "
            "a double sums exactly, so the drift this reintroduces has nothing to show up in",
        file=ACCOUNT_CPP,
        old="""        total = Volume::fromRaw(checkedAddI64(total.raw(), f.notional.raw()));""",
        new="""        total = Volume::fromDouble(total.toDouble() + f.notional.toDouble());""",
        targets=CLEARING_TARGETS,
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
        code, output = run_test(target, None)
        state = "green" if code == 0 else "RED"
        print(f"  control {target:<34} {state}   {summary_line(output).strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> str:
    """'killed', 'alive' or 'no-compile'."""
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
            narrow = m.gtest_filter if target == m.filter_target else None
            code, test_output = run_test(target, narrow)
            red = code != 0
            flt = f" --gtest_filter={narrow}" if narrow else ""
            print(f"  {target}{flt} -> exit {code} "
                  f"({'RED, mutation killed' if red else 'green'})")
            if red:
                verdict = "killed"
                failed = [line for line in test_output.splitlines()
                          if line.startswith("[  FAILED  ]")]
                for line in failed[:6]:
                    print(f"    {line}")
                continue

            if narrow:
                # Narrow filter stayed green -- fall back to the whole binary
                # in case another test in the same file catches it.
                code2, test_output2 = run_test(target, None)
                red2 = code2 != 0
                print(f"  {target} (whole binary fallback) -> exit {code2} "
                      f"({'RED, mutation killed' if red2 else 'green'})")
                if red2:
                    verdict = "killed"
                    failed = [line for line in test_output2.splitlines()
                              if line.startswith("[  FAILED  ]")]
                    for line in failed[:6]:
                        print(f"    {line}")

        if verdict == "alive":
            if m.equivalent_reason:
                print("  GREEN, MUTATION SURVIVED -- claimed equivalent (see reason above)")
            else:
                print("  GREEN, MUTATION SURVIVED every binary asked")
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
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            eq = "  [equivalence claimed]" if m.equivalent_reason else ""
            print(f"{m.name:<42} {', '.join(m.files())}{eq}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD} is not configured; run\n  cmake -S . -B build "
                         f"-DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON "
                         f"-DFLOX_ENABLE_BACKTEST=ON")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = all_targets(selected)

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD"}
    for m, verdict in results:
        eq = "  (claimed equivalent)" if (verdict == "alive" and m.equivalent_reason) else ""
        print(f"  {label[verdict]}  {m.name:<42} {', '.join(m.files())}{eq}")

    survived = [m.name for m, v in results if v == "alive" and not m.equivalent_reason]
    equivalent = [m.name for m, v in results if v == "alive" and m.equivalent_reason]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if equivalent:
        print(f"{len(equivalent)} mutation(s) survived but are claimed equivalent: "
              f"{', '.join(equivalent)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
