#!/usr/bin/env python3
"""Mutation harness for the backtest fill model.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the fill-model fix one piece at
a time, in the source, and checks that the tests written for it go red -- and
that an unmutated tree goes green before and after.

Two binaries answer for every mutation, and a mutation is killed if either of
them goes red:

    test_backtest_fill_realism   the acceptance tests
    test_backtest_fill_model     the tests that came with the fix

Each mutation names the tests that are supposed to notice it; that filter runs
first, and a mutation the filter does not kill is then re-run against the whole
of both binaries. A mutation still alive after that is swept against every
backtest binary in the tree before it is reported green, so a survivor is a
survivor of everything, not just of the two suites written for this change.

Every run is honest about the build: the mutated file's hash is printed before
and after, the library object for the mutated file and every object of every
target asked are deleted so nothing can be served from cache, the rebuild
output has to contain "Building CXX" or the run is refused, and each binary
runs under a timeout. A mutation that does not compile is not a mutation and is
reported as such.

Usage:

    python3 scripts/mutations/backtest_fills.py           # control, mutations, control
    python3 scripts/mutations/backtest_fills.py --list
    python3 scripts/mutations/backtest_fills.py --only ladder-is-not-consumed

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
TEST_TIMEOUT = 180

EXEC_CPP = "src/backtest/simulated_executor.cpp"
RUNNER_CPP = "src/backtest/backtest_runner.cpp"
TRACKER_H = "include/flox/backtest/order_queue_tracker.h"

REALISM = "test_backtest_fill_realism"
MODEL = "test_backtest_fill_model"
BOTH = [REALISM, MODEL]

# Every backtest binary in the tree. A mutation that survives BOTH is run
# against all of these before it is called green.
SWEEP = [
    "test_backtest",
    "test_backtest_ctx_position",
    "test_backtest_fee_attribution",
    "test_backtest_fill_model",
    "test_backtest_fill_realism",
    "test_backtest_metrics",
    "test_backtest_queue",
    "test_backtest_run_tape",
    "test_backtest_runner_hooks",
    "test_backtest_runner_venue_stack",
    "test_backtest_slippage",
    "test_liquidation_engine",
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
    targets: list[str] = field(default_factory=lambda: list(BOTH))
    # The tests that are supposed to notice. Run first; the whole binary runs
    # after it as the fallback.
    gtest_filter: str | None = None
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


MUTATIONS: list[Mutation] = [
    # ---- 1. the bar-callback hold window ----------------------------------
    Mutation(
        name="hold-window-never-opens",
        why="the window is ignored on the submit side, so an order emitted from a bar "
            "callback is matched inside the callback again -- against a bar that has "
            "already happened",
        file=EXEC_CPP,
        old="""  if (_barCallbackDepth > 0)
  {
    // The strategy is inside a bar callback, so every price of that bar --
    // open, high, low and close -- is already in the market state. Matching
    // here would hand the order a price it only exists to react to. Hold it;
    // the next bar's open releases it through this same path.
    _heldBarOrders.push_back(order);
    return;
  }

""",
        new="",
        gtest_filter="BacktestFillRealism.CallbackOrder*:BacktestFillModel.*Callback*",
    ),
    Mutation(
        name="hold-released-at-the-previous-close",
        why="the held orders are released before the open reaches the market state, so "
            "they trade at the close of the bar the strategy was shown -- the deferral "
            "costs a line of code and buys nothing",
        file=EXEC_CPP,
        old="""  setBarMarketState(symbol, open);
  releaseHeldOrders(symbol);""",
        new="""  releaseHeldOrders(symbol);
  setBarMarketState(symbol, open);""",
        gtest_filter="BacktestFillRealism.CallbackOrder*",
    ),
    Mutation(
        name="one-symbols-open-releases-every-hold",
        why="the hold list is drained whatever symbol the bar belongs to, so an order "
            "for a symbol with no bar is matched against another instrument's open",
        file=EXEC_CPP,
        old="    if (_heldBarOrders[i].symbol == symbol)\n    {\n      released.push_back",
        new="    (void)symbol;  // any symbol's bar releases every held order\n"
            "    if (true)\n    {\n      released.push_back",
        gtest_filter="BacktestFillModel.HeldOrderWaitsForItsOwnSymbolsOpen",
    ),
    Mutation(
        name="held-order-released-around-the-submit-path",
        why="a released order is pushed straight into finishSubmission, so the rate "
            "limit, reduce-only and self-trade prevention that the open is supposed to "
            "evaluate it against are all skipped -- the hold turns into a way past the "
            "venue's own gates",
        file=EXEC_CPP,
        old="""    // Full submit path: the order reaches the venue now, so rate limits,
    // reduce-only and self-trade prevention are all evaluated against the
    // state at the open rather than the state the strategy saw.
    submitOrder(order);""",
        new="""    Order accepted = order;
    accepted.createdAt = fromUnixNs(_clock.nowNs());
    emitEvent(OrderEventStatus::SUBMITTED, accepted);
    finishSubmission(std::move(accepted), /*fromAck=*/false);""",
        gtest_filter="BacktestFillRealism.CallbackOrder*:BacktestFillModel.*",
    ),
    Mutation(
        name="cancel-inside-the-callback-does-not-reach-the-hold-list",
        why="an order submitted and pulled inside one callback is no longer found where "
            "it waits, so the cancel misses it and the next bar's open releases an order "
            "the strategy already took back",
        file=EXEC_CPP,
        old="""  // An order submitted and pulled inside the same bar callback never reached
  // the venue. Drop it where it waits, or the next bar's open would release
  // an order the strategy has already cancelled.
  for (size_t i = 0; i < _heldBarOrders.size(); ++i)
  {
    if (_heldBarOrders[i].id == orderId)
    {
      const Order cancelled = _heldBarOrders[i];
      _heldBarOrders.erase(_heldBarOrders.begin() + static_cast<std::ptrdiff_t>(i));
      emitEvent(OrderEventStatus::CANCELED, cancelled);
      return;
    }
  }

""",
        new="",
        gtest_filter="BacktestFillModel.OrderCancelledInsideTheSameCallbackNeverFills",
    ),
    Mutation(
        name="held-orders-released-in-reverse-arrival-order",
        why="two orders held from the same callback reach the venue back to front, so "
            "whichever of them the queue, the position or self-trade prevention resolves "
            "first is decided by the hold list instead of by the strategy",
        file=EXEC_CPP,
        old="""  for (const Order& order : released)
  {""",
        new="""  std::reverse(released.begin(), released.end());
  for (const Order& order : released)
  {""",
        gtest_filter="BacktestFillRealism.CallbackOrder*:BacktestFillModel.*",
    ),
    Mutation(
        name="cancel-all-orders-misses-the-held-ones",
        why="cancelAllOrders stops sweeping the hold list, so an order the strategy "
            "cancelled along with everything else is still released at the next open",
        file=EXEC_CPP,
        old="""  // Orders still held for the next bar's open are part of "all orders" as far
  // as the strategy is concerned -- it submitted them and never got them back.
  while (i < _heldBarOrders.size())
  {
    if (_heldBarOrders[i].symbol == symbol)
    {
      canceled.push_back(_heldBarOrders[i]);
      _heldBarOrders.erase(_heldBarOrders.begin() + static_cast<std::ptrdiff_t>(i));
    }
    else
    {
      ++i;
    }
  }

  i = 0;
""",
        new="",
        gtest_filter="BacktestFillRealism.CallbackOrder*:BacktestFillModel.*",
    ),
    Mutation(
        name="window-closes-before-the-subscribers-run",
        why="the runner closes the bar-callback window before the execution subscribers "
            "see the bar, so an order a subscriber emits is matched inside the bar it "
            "was shown -- the look-ahead the window exists to remove, one listener down",
        file=RUNNER_CPP,
        old="""    for (auto* sub : _marketDataSubscribers)
    {
      sub->onBar(ev);
    }
    sim().endBarCallbackWindow();""",
        new="""    sim().endBarCallbackWindow();
    for (auto* sub : _marketDataSubscribers)
    {
      sub->onBar(ev);
    }""",
        gtest_filter="BacktestFillRealism.CallbackOrder*:BacktestFillModel.*",
    ),
    Mutation(
        name="window-left-open-when-there-is-no-strategy",
        why="the window is opened on every bar and closed only when a strategy is "
            "attached, so a strategy-less run leaks one level of depth per bar and every "
            "order submitted from anywhere afterwards is held forever -- the counter is "
            "not paired, and nothing asks whether it is",
        file=RUNNER_CPP,
        old="""    for (auto* sub : _marketDataSubscribers)
    {
      sub->onBar(ev);
    }
    sim().endBarCallbackWindow();""",
        new="""    for (auto* sub : _marketDataSubscribers)
    {
      sub->onBar(ev);
    }
    if (_strategy)
    {
      sim().endBarCallbackWindow();
    }""",
        gtest_filter="BacktestFillRealism.CallbackOrder*:BacktestFillModel.*",
    ),
    # ---- 2. the depth walk ------------------------------------------------
    Mutation(
        name="ladder-is-not-consumed",
        why="the walk reads the ladder but never takes anything off it, so the next "
            "order in the same step is handed the depth the first one already ate",
        file=EXEC_CPP,
        old="""      left -= take;
      levelQtyRaw -= take;""",
        new="      left -= take;",
        gtest_filter="BacktestFillRealism.ConsumedDepthIsGoneForTheNextOrder",
    ),
    Mutation(
        name="ladder-consumed-but-the-touch-is-not-republished",
        why="the walk eats the levels and leaves the market state quoting a touch that "
            "is no longer there, so every crossing decision after it is answered by a "
            "price with nothing behind it",
        file=EXEC_CPP,
        old="""  // Republish the touch the walk left behind. The market state and the ladder
  // have to agree, or the next order's crossing check would be answered by a
  // level that is no longer there.
  MarketState& state = getMarketState(symbol);
  const bool empty = levels.empty();
  if (side == Side::BUY)
  {
    state.hasAsk = !empty;
    state.bestAskRaw = empty ? 0 : levels.front().first;
    state.bestAskQtyRaw = empty ? 0 : levels.front().second;
  }
  else
  {
    state.hasBid = !empty;
    state.bestBidRaw = empty ? 0 : levels.front().first;
    state.bestBidQtyRaw = empty ? 0 : levels.front().second;
  }
  return walk;""",
        new="  return walk;",
        gtest_filter="BacktestFillModel.PartialLevelLeavesTheRestOfItAtTheTouch:"
                     "BacktestFillModel.ExhaustedSideReportsNoTouch",
    ),
    Mutation(
        name="walk-notional-computed-in-double",
        why="the notional goes through a double instead of the widened "
            "Quantity * Price -> Volume path, which is the arithmetic the codebase "
            "forbids for money: at crypto notionals the VWAP comes back wrong in the "
            "last places and every fill price is off by it",
        file=EXEC_CPP,
        old="""      walk.notional = walk.notional + (Quantity::fromRaw(take) * Price::fromRaw(priceRaw));""",
        new="""      walk.notional = walk.notional + Volume::fromDouble(Quantity::fromRaw(take).toDouble() *
                                                        Price::fromRaw(priceRaw).toDouble());""",
        gtest_filter="BacktestFillRealism.MarketBuyWalksTheAskLadder:"
                     "BacktestFillRealism.MarketSellWalksTheBidLadder:"
                     "BacktestFillRealism.RoutedLiquidationWalksTheBook:"
                     "BacktestFillModel.PartialLevelLeavesTheRestOfItAtTheTouch",
    ),
    Mutation(
        name="size-past-the-ladder-averaged-over-the-visible-levels",
        why="size the ladder cannot cover is averaged in at the levels that were "
            "visible, so the unseen remainder prints at a blend of the touch -- size is "
            "cheap again, which is the bug the walk replaced",
        file=EXEC_CPP,
        old="""      fillPriceRaw = (walk.takenRaw >= remainingQty.raw())
                         ? (walk.notional / Quantity::fromRaw(walk.takenRaw)).raw()
                         : walk.worstPriceRaw;""",
        new="""      fillPriceRaw = (walk.notional / Quantity::fromRaw(walk.takenRaw)).raw();""",
        gtest_filter="BacktestFillRealism.SizePastTheLadderIsNotFilledAtTheTouch:"
                     "BacktestFillModel.SizePastTheLadderPaysTheDeepestLevelForTheWholeOrder",
    ),
    Mutation(
        name="deepest-level-rule-applied-only-to-buys",
        why="a sell past the visible ladder averages over the levels it could see while "
            "a buy pays the deepest one: the same order costs different money depending "
            "on which way it points",
        file=EXEC_CPP,
        old="""      fillPriceRaw = (walk.takenRaw >= remainingQty.raw())""",
        new="""      fillPriceRaw = (walk.takenRaw >= remainingQty.raw() || order.side == Side::SELL)""",
        gtest_filter="BacktestFillRealism.SizePastTheLadderIsNotFilledAtTheTouch:"
                     "BacktestFillRealism.MarketSellWalksTheBidLadder:"
                     "BacktestFillRealism.RoutedLiquidationWalksTheBook:"
                     "BacktestFillModel.SizePastTheLadderPaysTheDeepestLevelForTheWholeOrder",
    ),
    Mutation(
        name="bar-step-keeps-the-stale-ladder",
        why="a bar carries no depth, but the ladder from the last book update is kept, "
            "so an order on bar data walks a book whose prices the bar has long left "
            "behind",
        file=EXEC_CPP,
        old="""  // A bar reports no depth. Dropping the ladder keeps a stale book from being
  // walked at prices this bar has already left behind: on bar data every size
  // trades at the step price, which is the only thing the bar says.
  _ladders.erase(symbol);

""",
        new="",
        gtest_filter="BacktestFillModel.BarStepDropsTheStaleLadder",
    ),
    # ---- 3. the trigger bound ---------------------------------------------
    Mutation(
        name="trigger-bound-dropped",
        why="a triggered conditional is matched against the bar extreme that armed the "
            "step, so a take-profit books the high it was never entitled to",
        file=EXEC_CPP,
        old="""  if (triggerBoundRaw != 0)
  {
    fillPriceRaw = (order.side == Side::BUY) ? std::max(fillPriceRaw, triggerBoundRaw)
                                             : std::min(fillPriceRaw, triggerBoundRaw);
  }

""",
        new="",
        gtest_filter="BacktestFillRealism.TakeProfit*",
    ),
    Mutation(
        name="trigger-bound-applied-to-the-wrong-side",
        why="the one-sided bound is mirrored: a buy may not pay less than its trigger "
            "and a sell may not sell above it, which turns a protective stop into a fill "
            "better than the price it was armed at",
        file=EXEC_CPP,
        old="""    fillPriceRaw = (order.side == Side::BUY) ? std::max(fillPriceRaw, triggerBoundRaw)
                                             : std::min(fillPriceRaw, triggerBoundRaw);""",
        new="""    fillPriceRaw = (order.side == Side::BUY) ? std::min(fillPriceRaw, triggerBoundRaw)
                                             : std::max(fillPriceRaw, triggerBoundRaw);""",
        gtest_filter="BacktestFillRealism.TakeProfit*:"
                     "BacktestFillRealism.StopSellStillPaysTheAdverseExtreme:"
                     "BacktestFillRealism.TriggeredStopLimitDoesNotPrintTheBarHigh",
    ),
    Mutation(
        name="trigger-bound-checked-after-the-depth-walk",
        why="the stop-limit that cannot trade at the price it was armed at is refused "
            "only after the walk has already taken its size off the ladder: the order "
            "rests, and the depth it never traded against is gone for everyone else",
        file=EXEC_CPP,
        edits=[
            Edit(
                file=EXEC_CPP,
                old="""  // A stop-limit armed at a price past its own limit cannot trade there: it
  // rests instead, which is what a venue does with a stop-limit triggered on a
  // gap. Decided before the walk, so a refused fill does not eat depth on its
  // way out.
  if (triggerBoundRaw != 0 && order.type != OrderType::MARKET)
  {
    const bool beyondLimit =
        (order.side == Side::BUY && triggerBoundRaw > order.price.raw()) ||
        (order.side == Side::SELL && triggerBoundRaw < order.price.raw());
    if (beyondLimit)
    {
      return false;
    }
  }

""",
                new="",
            ),
            Edit(
                file=EXEC_CPP,
                old="""  // A conditional order may not print better than the price that armed it.""",
                new="""  if (triggerBoundRaw != 0 && order.type != OrderType::MARKET)
  {
    const bool beyondLimit =
        (order.side == Side::BUY && triggerBoundRaw > order.price.raw()) ||
        (order.side == Side::SELL && triggerBoundRaw < order.price.raw());
    if (beyondLimit)
    {
      return false;
    }
  }

  // A conditional order may not print better than the price that armed it.""",
            ),
        ],
        gtest_filter="BacktestFillRealism.TriggeredStopLimitDoesNotPrintTheBarHigh:"
                     "BacktestFillModel.TriggeredStopLimitFillsWithAQueueModelOn",
    ),
    # ---- 4. the queue model on bars ---------------------------------------
    Mutation(
        name="queue-print-sized-to-the-first-order-only",
        why="the synthetic print keeps the reach of whichever of our orders was seen "
            "first at the level instead of the one standing furthest back, so an order "
            "deeper in the same queue never gets its turn however far the bar trades "
            "through it",
        file=EXEC_CPP,
        old="""    else if (reachRaw > existing->qtyRaw)
    {
      existing->qtyRaw = reachRaw;
    }
""",
        new="",
        gtest_filter="BacktestFillRealism.RestingLimitFillsOnBarDataWithQueueModel:"
                     "BacktestFillRealism.VenueStackPresetFillsRestingLimitOnBarData",
    ),
    Mutation(
        name="queue-print-emitted-on-a-touch",
        why="a bar that reaches our price without trading through it is taken as proof "
            "the queue ahead is gone, so every resting limit fills on every bar that "
            "brushes it -- which is not a queue model",
        file=EXEC_CPP,
        old="""    const bool tradedThrough = (order.side == Side::BUY) ? (stepRaw < order.price.raw())
                                                         : (stepRaw > order.price.raw());""",
        new="""    const bool tradedThrough = (order.side == Side::BUY) ? (stepRaw <= order.price.raw())
                                                         : (stepRaw >= order.price.raw());""",
        gtest_filter="BacktestFillRealism.BarThatOnlyTouchesThePriceLeavesTheQueueUnfilled",
    ),
    Mutation(
        name="queue-print-ignores-the-queue-ahead",
        why="the print is sized to our own remainder and not to the queue standing in "
            "front of it, so a bar that traded a point through the level still leaves "
            "the order waiting behind size that has already traded",
        file=EXEC_CPP,
        old="""    const int64_t reachRaw = snap->ahead.raw() + remainingRaw;""",
        new="""    const int64_t reachRaw = remainingRaw;""",
        gtest_filter="BacktestFillRealism.RestingLimitFillsOnBarDataWithQueueModel:"
                     "BacktestFillRealism.VenueStackPresetFillsRestingLimitOnBarData",
    ),
    Mutation(
        name="pending-orders-skip-every-limit-again",
        why="the skip goes back to asking whether the model is on instead of whether "
            "the tracker actually holds the order, so a limit the tracker never took -- "
            "a stop-limit that rested when it triggered -- waits for a queue it is not in",
        file=EXEC_CPP,
        old="""    if (order.type == OrderType::LIMIT && _queueTracker.enabled() &&
        _queueTracker.snapshot(order.id).has_value())""",
        new="""    if (order.type == OrderType::LIMIT && _queueTracker.enabled())""",
        gtest_filter="BacktestFillModel.TriggeredStopLimitFillsWithAQueueModelOn",
    ),
    # ---- 5. reset and the second run --------------------------------------
    Mutation(
        name="reset-does-not-clear-the-fills",
        why="the fill vector survives the reset, so the second run reports the sum of "
            "every run so far -- the finding the reset exists for",
        file=EXEC_CPP,
        old="""  _fills.clear();
""",
        new="",
        gtest_filter="BacktestFillRealism.SecondRun*:"
                     "BacktestFillModel.ResetClearsFillsAndOrdersButKeepsConfiguration",
    ),
    Mutation(
        name="reset-does-not-restore-the-seeded-rngs",
        why="the seeded generators are left wherever the previous run stopped, so two "
            "runs of the same data over the same seed draw different cancel-ack and "
            "iceberg-jitter sequences and a backtest stops being reproducible",
        file=EXEC_CPP,
        old="""  _cancelAckRng.seed(_cancelAckSeed);
  _icebergJitterRng.seed(_icebergJitterSeed);
""",
        new="",
        gtest_filter="BacktestFillRealism.SecondRun*:"
                     "BacktestFillModel.ResetClearsFillsAndOrdersButKeepsConfiguration",
    ),
    Mutation(
        name="reset-clears-the-configuration",
        why="the reset throws away the queue model the caller installed, so the second "
            "run is run under a different simulator than the first one",
        file=EXEC_CPP,
        old="""  _compositeLogic = CompositeOrderLogic{0};""",
        new="""  setQueueModel(QueueModel::NONE, 0);
  _compositeLogic = CompositeOrderLogic{0};""",
        gtest_filter="BacktestFillModel.ResetClearsFillsAndOrdersButKeepsConfiguration:"
                     "BacktestFillRealism.RestingLimit*",
    ),
    Mutation(
        name="reset-keeps-the-held-orders",
        why="orders still waiting for an open that never came are carried into the next "
            "run, where the first bar releases them: run two opens with a position run "
            "one decided on",
        file=EXEC_CPP,
        old="""  _heldBarOrders.clear();
  _barCallbackDepth = 0;
""",
        new="",
        gtest_filter="BacktestFillRealism.SecondRun*:BacktestFillModel.*",
    ),
    Mutation(
        name="second-run-does-not-reset",
        why="runBars stops clearing the previous run, which is the finding: the second "
            "result is the sum of both runs and carries the first run's timestamps",
        file=RUNNER_CPP,
        old="""BacktestResult BacktestRunner::runBars(const std::vector<BarEvent>& bars)
{
  resetRunState();""",
        new="""BacktestResult BacktestRunner::runBars(const std::vector<BarEvent>& bars)
{""",
        gtest_filter="BacktestFillRealism.SecondRun*",
    ),
    Mutation(
        name="only-run-bars-resets",
        why="the bar path is cleaned and the two paths the tests do not re-run -- "
            "run() over a reader and the interactive start() -- are not, so the fix "
            "holds exactly where it is measured",
        edits=[
            Edit(
                file=RUNNER_CPP,
                old="""BacktestResult BacktestRunner::run(replay::IMultiSegmentReader& reader)
{
  resetRunState();""",
                new="""BacktestResult BacktestRunner::run(replay::IMultiSegmentReader& reader)
{""",
            ),
            Edit(
                file=RUNNER_CPP,
                old="""void BacktestRunner::start(replay::IMultiSegmentReader& reader)
{
  resetRunState();""",
                new="""void BacktestRunner::start(replay::IMultiSegmentReader& reader)
{""",
            ),
        ],
        gtest_filter="BacktestFillRealism.SecondRun*:BacktestFillModel.*",
    ),
    Mutation(
        name="queue-tracker-clear-does-nothing",
        why="the tracker keeps every level and every resting order across the reset, so "
            "the next run's orders queue up behind size that traded in the previous one",
        file=TRACKER_H,
        old="""  void clear() { _levels.clear(); }""",
        new="""  void clear() {}""",
        gtest_filter="BacktestFillModel.ResetClearsFillsAndOrdersButKeepsConfiguration:"
                     "BacktestFillRealism.SecondRun*",
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
    rebuilt rather than trusting the dependency scanner.
    """
    if source.endswith((".h", ".inl", ".hpp")):
        return find_objects("-name", "*.o", "-path", f"*{LIB}.dir*")
    return find_objects("-name", f"{Path(source).name}.o", "-path", f"*{LIB}.dir*")


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


def build_all(targets: list[str], sources: list[str]) -> None:
    """Delete every object that could hide the mutation, then rebuild."""
    removed = []
    for source in sources:
        removed += library_objects(source)
    for target in targets:
        removed += target_objects(target)
    for obj in dict.fromkeys(removed):
        obj.unlink()
    # The previous mutation's restore already deleted these, so a zero here is
    # the expected steady state; the "Building CXX" count below is the proof
    # that the mutated source was actually compiled.
    print(f"  removed {len(set(removed))} object file(s) "
          f"(0 = already deleted by the previous restore)")
    output = rebuild(LIB)
    compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
    print(f"  rebuilt {LIB}: {compiled} 'Building CXX' line(s)")
    for target in targets:
        out = rebuild(target)
        n = sum(1 for line in out.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {target}: {n} 'Building CXX' line(s)")


def run_mutation(m: Mutation) -> str:
    """'killed', 'alive' or 'no-compile'."""
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
        try:
            build_all(m.targets, m.files())
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

        # The whole of both binaries, always: it says which suite actually
        # answers for the mutation, which is the point of asking two of them.
        if m.gtest_filter and not filterKilled:
            print("  the named tests did not notice; falling back to the whole binaries")
        red: list[str] = []
        for target in m.targets:
            code, output = run_test(target, None)
            report(target, None, code, output)
            if code:
                red.append(target)
                verdict = "killed"
        if len(red) == 1:
            print(f"  only {red[0]} answers for this one")

        # Still alive: sweep it against every backtest binary in the tree
        # before calling it green.
        if verdict == "alive":
            print("  sweeping every backtest binary")
            extra = [t for t in SWEEP if t not in m.targets]
            try:
                for target in extra:
                    # The mutation is in the library, whose object for the
                    # mutated file was already deleted and recompiled above, so
                    # these binaries only have to be linked against it again --
                    # their own sources did not change.
                    out = rebuild(target, requireCompile=False)
                    if "Linking CXX" not in out and "Building CXX" not in out:
                        raise SystemExit(
                            f"{target} was neither compiled nor linked after the "
                            f"library changed; the binary would be stale:\n{out[-800:]}")
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
        for target in m.targets:
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
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD"}
    for m, verdict in results:
        print(f"  {label[verdict]}  {m.name:<52} {', '.join(m.files())}")
    survived = [m.name for m, v in results if v == "alive"]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
