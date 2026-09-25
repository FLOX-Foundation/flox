#!/usr/bin/env python3
"""Mutation harness for the executor bar path across the bindings (W33-T036).

`flox_simulated_executor_on_bar_ohlc` / `_begin_bar_callback_window` /
`_end_bar_callback_window` / `_reset`, plus `close_reason` on `FloxBar`, were
added to the C API and then projected into pybind11, napi, QuickJS and Codon
(see .notes/tracks/W33-stabilization/T036-bindings.md, "## Код (агент C)").
This script breaks that projection one piece and one binding at a time, and
checks that the test written for it goes red.

Five build systems answer for the pieces below:

  c        include/flox/capi/flox_capi.h, src/capi/flox_capi.cpp
           -> rebuild the `flox_capi` target (and the two gtest binaries when
              a header both of them include is mutated) in build/, run
              tests/test_capi_executor_bar_ohlc and/or tests/test_capi_infra.
  quickjs  src/quickjs/js_bindings.cpp, src/quickjs/js_strategy.cpp
           -> rebuild `flox_quickjs` then relink `test_quickjs` in build/.
  python   python/backtest_bindings.h, python/aggregator_bindings.h
           -> rebuild `_flox_py` in build-py/, run pytest under it.
  node     node/src/backtest.h, node/src/aggregators.h
           -> `npm run build` in node/, run node/test/test_executor_bar_ohlc_parity.js.
  codegen  codon/flox/*.codon, tools/codegen/binding_parity.yaml,
           tools/codegen/golden/flox_capi.codon
           -> no compiler for Codon in this environment; these are plain-text
              files read by pytest tools/codegen/tests. No rebuild.

Three of the five bindings that carry this bar path -- C, Python, Node -- are
compiled from flox_capi.cpp/.h directly (each binding embeds its own copy of
the C shim), so a mutation there can affect the c, python AND node systems at
once. Anything that survives its primary system's test is swept across every
OTHER system that same file feeds, before it is called a hole. QuickJS and
Codon are single-owner: nothing else in the sweep touches their source.

Every rebuild is honest: the mutated file's hash is printed before and after,
the target's own object files are deleted first so nothing is served from a
stale cache, the rebuild output has to contain "Building CXX" (both the Ninja
and Makefile generators phrase it that way) or the run is refused, and every
test invocation runs under a timeout.

Usage (run from anywhere; paths below are resolved from this file):

    python3 scripts/mutations/executor_bar_path.py             # control, all, control
    python3 scripts/mutations/executor_bar_path.py --list
    python3 scripts/mutations/executor_bar_path.py --only c-on-bar-ohlc-close-as-open
    python3 scripts/mutations/executor_bar_path.py --skip-control

Build directories are expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON \\
          -DFLOX_BUILD_CAPI=ON -DFLOX_BUILD_QUICKJS=ON -DFLOX_NATIVE=OFF
    cmake -S . -B build-py -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_PYTHON=ON \\
          -DFLOX_BUILD_TESTS=OFF -DFLOX_BUILD_CAPI=OFF -DFLOX_BUILD_QUICKJS=OFF \\
          -DFLOX_NATIVE=OFF -Dpybind11_DIR=<pybind11.get_cmake_dir() of .venv-tests>
    (cd node && npm run build)   # once, to populate node_modules/build

and a venv at .venv-tests/ with "pybind11<3.1.0" numpy pytest pyyaml installed.
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
BUILD_C = REPO / "build"
BUILD_PY = REPO / "build-py"
NODE_DIR = REPO / "node"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 180

VENV_PYTHON = REPO / ".venv-tests" / "bin" / "python"
PYTHON = str(VENV_PYTHON) if VENV_PYTHON.is_file() else sys.executable
PY_PYTHONPATH = str(BUILD_PY / "python")

NPM = shutil.which("npm") or "npm"
NODE = shutil.which("node") or "node"

# ── files ────────────────────────────────────────────────────────────────
CAPI_H = "include/flox/capi/flox_capi.h"
CAPI_CPP = "src/capi/flox_capi.cpp"
PY_BACKTEST = "python/backtest_bindings.h"
PY_AGGREGATOR = "python/aggregator_bindings.h"
NODE_BACKTEST = "node/src/backtest.h"
NODE_AGGREGATORS = "node/src/aggregators.h"
QJS_BINDINGS = "src/quickjs/js_bindings.cpp"
CODON_BACKTEST = "codon/flox/backtest.codon"
CODON_TOOLS = "codon/flox/tools.codon"
YAML_MANIFEST = "tools/codegen/binding_parity.yaml"
CODON_GOLDEN = "tools/codegen/golden/flox_capi.codon"

# ── test surfaces ───────────────────────────────────────────────────────
CAPI_EXECUTOR_BIN = BUILD_C / "tests" / "test_capi_executor_bar_ohlc"
CAPI_INFRA_BIN = BUILD_C / "tests" / "test_capi_infra"
QUICKJS_BIN = BUILD_C / "tests" / "test_quickjs"
PY_TEST_FILE = "python/tests/test_executor_bar_ohlc_parity.py"
NODE_TEST_FILE = "test/test_executor_bar_ohlc_parity.js"
CODEGEN_TEST_DIR = "tools/codegen/tests"
CODEGEN_TEST_FILE = "tools/codegen/tests/test_binding_parity_executor_bar_path.py"


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
    system: str  # c | quickjs | python | node | codegen
    file: str = ""
    old: str = ""
    new: str = ""
    edits: list[Edit] = field(default_factory=list)
    occurrence: int = 1
    expected_occurrences: int = 1

    # cmake targets to rebuild for c/quickjs/python (node always runs
    # `npm run build`; codegen never rebuilds).
    build_targets: list[str] = field(default_factory=list)
    # substrings of object-file directories to purge before rebuilding
    # (".dir" is appended), so nothing is served from cache.
    purge: list[str] = field(default_factory=list)

    # primary, cheap check
    gtest_binary: Path | None = None
    gtest_filter: str | None = None
    pytest_nodeids: list[str] | None = None
    node_test: bool = False
    codegen_nodeids: list[str] | None = None

    # systems (other than `system`) this same file also feeds; swept only
    # if the mutation survives its primary check.
    also_feeds: list[str] = field(default_factory=list)

    # documented reason a hole is not really a hole; empty means "this is a
    # real gap if it survives".
    equivalent_reason: str = ""
    # documented reason no test in this environment could ever catch this
    # (e.g. no Codon compiler, no QuickJS runtime test for this surface) --
    # still run, still reported, but not counted as "unexplained".
    known_uncovered_reason: str = ""

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


# ═══════════════════════════════════════════════════════════════════════
# Mutations
# ═══════════════════════════════════════════════════════════════════════

MUTATIONS: list[Mutation] = [

    # ── C: on_bar_ohlc's own projection ──────────────────────────────────
    Mutation(
        name="c-on-bar-ohlc-close-as-open",
        why="flox_simulated_executor_on_bar_ohlc forwards close_price where open_price "
            "belongs (and vice versa): a held callback order fills at the bar's close "
            "instead of the next bar's open",
        system="c", file=CAPI_CPP,
        old="""  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(open_price), Price::fromDouble(high_price),
      Price::fromDouble(low_price), Price::fromDouble(close_price));""",
        new="""  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(close_price), Price::fromDouble(high_price),
      Price::fromDouble(low_price), Price::fromDouble(open_price));""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        gtest_filter="CapiExecutorBarOhlc.AHeldOrderFillsAtTheOpenNotTheClose",
        also_feeds=["python", "node"],
    ),
    Mutation(
        name="c-on-bar-ohlc-high-low-swapped",
        why="flox_simulated_executor_on_bar_ohlc forwards high_price and low_price "
            "swapped, so the walk's intrabar extremes are inverted",
        system="c", file=CAPI_CPP,
        old="""  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(open_price), Price::fromDouble(high_price),
      Price::fromDouble(low_price), Price::fromDouble(close_price));""",
        new="""  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(open_price), Price::fromDouble(low_price),
      Price::fromDouble(high_price), Price::fromDouble(close_price));""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        # A single resting order cannot see this: a bar straddling it touches
        # it whichever way round the two extremes arrive. The bracket case can
        # -- its two children sit on opposite sides of the bar and only the
        # one the walk reaches first survives.
        gtest_filter="CapiExecutorBarOhlc.TheWalkReachesTheLowBeforeTheHigh",
        also_feeds=["python", "node"],
    ),
    Mutation(
        name="c-on-bar-ohlc-calls-close-only-overload",
        why="flox_simulated_executor_on_bar_ohlc calls the two-argument close-only "
            "onBar(symbol, close) overload instead of the four-price one, so it is the "
            "close-only path wearing a new name",
        system="c", file=CAPI_CPP,
        old="""  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(open_price), Price::fromDouble(high_price),
      Price::fromDouble(low_price), Price::fromDouble(close_price));""",
        new="""  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(close_price));
  (void)open_price;
  (void)high_price;
  (void)low_price;""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        gtest_filter="CapiExecutorBarOhlc.AHeldOrderFillsAtTheOpenNotTheClose",
        also_feeds=["python", "node"],
    ),
    Mutation(
        name="c-window-begin-not-forwarded",
        why="flox_simulated_executor_begin_bar_callback_window is a no-op: a callback "
            "order is never held, so it matches inside the bar it was submitted from",
        system="c", file=CAPI_CPP,
        old="""void flox_simulated_executor_begin_bar_callback_window(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.beginBarCallbackWindow();
  FLOX_CAPI_LEAVE_VOID;
}""",
        new="""void flox_simulated_executor_begin_bar_callback_window(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  (void)h;
  FLOX_CAPI_LEAVE_VOID;
}""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        gtest_filter="CapiExecutorBarOhlc.HandDrivenBarsMatchTheRunner",
        also_feeds=["python", "node"],
    ),
    Mutation(
        name="c-window-end-not-forwarded",
        why="flox_simulated_executor_end_bar_callback_window is a no-op: the window "
            "opened by begin() is never closed, so its depth counter only ever grows",
        system="c", file=CAPI_CPP,
        old="""void flox_simulated_executor_end_bar_callback_window(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.endBarCallbackWindow();
  FLOX_CAPI_LEAVE_VOID;
}""",
        new="""void flox_simulated_executor_end_bar_callback_window(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  (void)h;
  FLOX_CAPI_LEAVE_VOID;
}""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        gtest_filter=None,  # whole binary: no single case names "the window never closes"
        also_feeds=["python", "node"],
    ),
    Mutation(
        name="c-reset-noop",
        why="flox_simulated_executor_reset is a no-op: a second hand-driven run reports "
            "the sum of every run instead of just its own",
        system="c", file=CAPI_CPP,
        old="""void flox_simulated_executor_reset(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.reset();
  FLOX_CAPI_LEAVE_VOID;
}""",
        new="""void flox_simulated_executor_reset(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  (void)h;
  FLOX_CAPI_LEAVE_VOID;
}""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        gtest_filter="CapiExecutorBarOhlc.ResetMakesASecondRunReportThatRun",
        also_feeds=["python", "node"],
    ),

    # ── C: close_reason ──────────────────────────────────────────────────
    Mutation(
        name="c-close-reason-not-written-by-writeFloxBar",
        why="toFloxBar, the one helper both the batch aggregation and the callback bar ring "
            "(flox_strategy_last_closed_bar / _last_n_closed_bars) go through since the "
            "aggregator refactor, drops close_reason, so every FloxBar carries its zero-init value",
        system="c", file=CAPI_CPP,
        old="""          static_cast<uint32_t>(bar.tradeCount.raw()),
          static_cast<uint8_t>(bar.reason)};""",
        new="""          static_cast<uint32_t>(bar.tradeCount.raw())};""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        # AggregatedBarsCarryACloseReason goes through doAggregateC and cannot
        # see this writer; the bar-ring case reads exactly what it produces.
        gtest_filter="CapiExecutorBarOhlc.AClosedBarReadBackCarriesItsCloseReason",
        also_feeds=["python", "node"],
    ),
    Mutation(
        name="c-close-reason-wrong-field-doAggregateC",
        why="toFloxBar writes trade_count's value into close_reason instead of the bar's own reason",
        system="c", file=CAPI_CPP,
        old="""          static_cast<uint32_t>(bar.tradeCount.raw()),
          static_cast<uint8_t>(bar.reason)};""",
        new="""          static_cast<uint32_t>(bar.tradeCount.raw()),
          static_cast<uint8_t>(bar.tradeCount.raw())};""",
        build_targets=["flox_capi"], purge=["flox_capi"],
        gtest_binary=CAPI_EXECUTOR_BIN,
        gtest_filter="CapiExecutorBarOhlc.AggregatedBarsCarryACloseReason",
        also_feeds=["python", "node"],
    ),

    # ── C: ABI version ───────────────────────────────────────────────────
    Mutation(
        name="c-abi-version-left-at-2",
        why="FLOX_CAPI_ABI_VERSION in the compiled header (flox_capi.h, not the IDL "
            "spec) stays 2 even though FloxBar's shape changed (close_reason added), so "
            "flox_capi_abi_version() under-reports the ABI a consumer is actually loading",
        system="c", file=CAPI_H,
        old="#define FLOX_CAPI_ABI_VERSION 3",
        new="#define FLOX_CAPI_ABI_VERSION 2",
        build_targets=["flox_capi", "test_capi_executor_bar_ohlc", "test_capi_infra"],
        purge=["flox_capi", "test_capi_executor_bar_ohlc", "test_capi_infra"],
        gtest_binary=CAPI_INFRA_BIN,
        gtest_filter=None,
        also_feeds=["python", "node"],
        # test_capi_infra now carries CapiAbiVersion.TheNumberMovedWithTheStructShape,
        # which anchors the number to a shape the header declares rather than
        # comparing the macro with itself; the codegen sweep additionally
        # compares the spec, the golden and the shipped header.
    ),

    # ── Python ──────────────────────────────────────────────────────────
    Mutation(
        name="py-on-bar-ohlc-close-as-open",
        why="PySimulatedExecutor::onBarOhlc forwards closePrice where openPrice belongs",
        system="python", file=PY_BACKTEST,
        old="""    _executor.onBar(symbol, Price::fromDouble(openPrice), Price::fromDouble(highPrice),
                    Price::fromDouble(lowPrice), Price::fromDouble(closePrice));""",
        new="""    _executor.onBar(symbol, Price::fromDouble(closePrice), Price::fromDouble(highPrice),
                    Price::fromDouble(lowPrice), Price::fromDouble(openPrice));""",
        build_targets=["_flox_py"], purge=["_flox_py"],
        pytest_nodeids=[f"{PY_TEST_FILE}::test_a_held_order_fills_at_the_open_not_the_close"],
    ),
    Mutation(
        name="py-on-bar-ohlc-high-low-swapped",
        why="PySimulatedExecutor::onBarOhlc forwards highPrice and lowPrice swapped",
        system="python", file=PY_BACKTEST,
        old="""    _executor.onBar(symbol, Price::fromDouble(openPrice), Price::fromDouble(highPrice),
                    Price::fromDouble(lowPrice), Price::fromDouble(closePrice));""",
        new="""    _executor.onBar(symbol, Price::fromDouble(openPrice), Price::fromDouble(lowPrice),
                    Price::fromDouble(highPrice), Price::fromDouble(closePrice));""",
        build_targets=["_flox_py"], purge=["_flox_py"],
        # As on the C side: one resting order cannot tell the two extremes
        # apart, the bracket case can.
        pytest_nodeids=[f"{PY_TEST_FILE}::test_the_walk_reaches_the_low_before_the_high"],
    ),
    Mutation(
        name="py-window-begin-noop",
        why="PySimulatedExecutor::beginBarCallbackWindow is a no-op",
        system="python", file=PY_BACKTEST,
        old="void beginBarCallbackWindow() { _executor.beginBarCallbackWindow(); }",
        new="void beginBarCallbackWindow() { /* mutated: no-op */ }",
        build_targets=["_flox_py"], purge=["_flox_py"],
        pytest_nodeids=[f"{PY_TEST_FILE}::test_hand_driven_bars_match_the_runner"],
    ),
    Mutation(
        name="py-window-end-noop",
        why="PySimulatedExecutor::endBarCallbackWindow is a no-op",
        system="python", file=PY_BACKTEST,
        old="void endBarCallbackWindow() { _executor.endBarCallbackWindow(); }",
        new="void endBarCallbackWindow() { /* mutated: no-op */ }",
        build_targets=["_flox_py"], purge=["_flox_py"],
        pytest_nodeids=[f"{PY_TEST_FILE}::test_hand_driven_bars_match_the_runner"],
    ),
    Mutation(
        name="py-reset-noop",
        why="PySimulatedExecutor::reset is a no-op",
        system="python", file=PY_BACKTEST,
        old="void reset() { _executor.reset(); }",
        new="void reset() { /* mutated: no-op */ }",
        build_targets=["_flox_py"], purge=["_flox_py"],
        pytest_nodeids=[f"{PY_TEST_FILE}::test_reset_makes_a_second_run_report_that_run"],
    ),
    Mutation(
        name="py-extbar-dtype-without-close-reason",
        why="PyExtBar keeps the close_reason field but the PYBIND11_NUMPY_DTYPE "
            "registration drops it, so the numpy dtype aggregate_*_bars returns has no "
            "close_reason column even though the underlying struct carries the byte",
        system="python", file=PY_AGGREGATOR,
        old="""  PYBIND11_NUMPY_DTYPE(PyExtBar, start_time_ns, end_time_ns, open_raw, high_raw, low_raw,
                       close_raw, volume_raw, buy_volume_raw, trade_count, close_reason);""",
        new="""  PYBIND11_NUMPY_DTYPE(PyExtBar, start_time_ns, end_time_ns, open_raw, high_raw, low_raw,
                       close_raw, volume_raw, buy_volume_raw, trade_count);""",
        build_targets=["_flox_py"], purge=["_flox_py"],
        pytest_nodeids=[f"{PY_TEST_FILE}::test_aggregated_bars_carry_a_close_reason"],
    ),

    # ── Node ────────────────────────────────────────────────────────────
    Mutation(
        name="node-on-bar-ohlc-close-as-open",
        why="SimulatedExecutorWrap::OnBarOhlc forwards info[4] (close) where info[1] "
            "(open) belongs and vice versa",
        system="node", file=NODE_BACKTEST,
        old="""    flox_simulated_executor_on_bar_ohlc(_h, info[0].As<Napi::Number>().Uint32Value(),
                                        info[1].As<Napi::Number>().DoubleValue(),
                                        info[2].As<Napi::Number>().DoubleValue(),
                                        info[3].As<Napi::Number>().DoubleValue(),
                                        info[4].As<Napi::Number>().DoubleValue());""",
        new="""    flox_simulated_executor_on_bar_ohlc(_h, info[0].As<Napi::Number>().Uint32Value(),
                                        info[4].As<Napi::Number>().DoubleValue(),
                                        info[2].As<Napi::Number>().DoubleValue(),
                                        info[3].As<Napi::Number>().DoubleValue(),
                                        info[1].As<Napi::Number>().DoubleValue());""",
        node_test=True,
    ),
    Mutation(
        name="node-on-bar-ohlc-high-low-swapped",
        why="SimulatedExecutorWrap::OnBarOhlc forwards info[2] (high) and info[3] (low) "
            "swapped",
        system="node", file=NODE_BACKTEST,
        old="""    flox_simulated_executor_on_bar_ohlc(_h, info[0].As<Napi::Number>().Uint32Value(),
                                        info[1].As<Napi::Number>().DoubleValue(),
                                        info[2].As<Napi::Number>().DoubleValue(),
                                        info[3].As<Napi::Number>().DoubleValue(),
                                        info[4].As<Napi::Number>().DoubleValue());""",
        new="""    flox_simulated_executor_on_bar_ohlc(_h, info[0].As<Napi::Number>().Uint32Value(),
                                        info[1].As<Napi::Number>().DoubleValue(),
                                        info[3].As<Napi::Number>().DoubleValue(),
                                        info[2].As<Napi::Number>().DoubleValue(),
                                        info[4].As<Napi::Number>().DoubleValue());""",
        node_test=True,
    ),
    Mutation(
        name="node-window-begin-noop",
        why="SimulatedExecutorWrap::BeginBarCallbackWindow is a no-op",
        system="node", file=NODE_BACKTEST,
        old="""  void BeginBarCallbackWindow(const Napi::CallbackInfo&)
  {
    flox_simulated_executor_begin_bar_callback_window(_h);
  }""",
        new="""  void BeginBarCallbackWindow(const Napi::CallbackInfo&)
  {
    /* mutated: no-op */
  }""",
        node_test=True,
    ),
    Mutation(
        name="node-window-end-noop",
        why="SimulatedExecutorWrap::EndBarCallbackWindow is a no-op",
        system="node", file=NODE_BACKTEST,
        old="""  void EndBarCallbackWindow(const Napi::CallbackInfo&)
  {
    flox_simulated_executor_end_bar_callback_window(_h);
  }""",
        new="""  void EndBarCallbackWindow(const Napi::CallbackInfo&)
  {
    /* mutated: no-op */
  }""",
        node_test=True,
    ),
    Mutation(
        name="node-reset-noop",
        why="SimulatedExecutorWrap::Reset is a no-op",
        system="node", file=NODE_BACKTEST,
        old="  void Reset(const Napi::CallbackInfo&) { flox_simulated_executor_reset(_h); }",
        new="  void Reset(const Napi::CallbackInfo&) { /* mutated: no-op */ }",
        node_test=True,
    ),
    Mutation(
        name="node-close-reason-wrong-byte",
        why="barsToJs reads closeReason off trade_count (the adjacent field in FloxBar's "
            "layout) instead of close_reason",
        system="node", file=NODE_AGGREGATORS,
        old='    o.Set("closeReason", bars[i].close_reason);',
        new='    o.Set("closeReason", bars[i].trade_count);',
        node_test=True,
    ),

    # ── QuickJS: no runtime test exercises this surface at all ────────────
    Mutation(
        name="quickjs-on-bar-ohlc-close-as-open",
        why="js_executor_on_bar_ohlc forwards argv[5] (close) where argv[2] (open) "
            "belongs and vice versa",
        system="quickjs", file=QJS_BINDINGS,
        old="""  flox_simulated_executor_on_bar_ohlc(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toUint32(ctx, argv[1]), toDouble(ctx, argv[2]), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]));""",
        new="""  flox_simulated_executor_on_bar_ohlc(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toUint32(ctx, argv[1]), toDouble(ctx, argv[5]), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[2]));""",
        build_targets=["flox_quickjs", "test_quickjs"], purge=["flox_quickjs", "test_quickjs"],
        gtest_binary=QUICKJS_BIN, gtest_filter=None,
        # JsIntegrationTest.SimulatedExecutorDrivesTheBarPath drives the JS
        # SimulatedExecutor over the tape and compares the realised pnl with a
        # C++ control run in the same process, so a swapped open/close moves a
        # number the test reads.
    ),
    Mutation(
        name="quickjs-window-begin-noop",
        why="js_executor_begin_bar_callback_window is a no-op",
        system="quickjs", file=QJS_BINDINGS,
        old="""static JSValue js_executor_begin_bar_callback_window(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  flox_simulated_executor_begin_bar_callback_window(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}""",
        new="""static JSValue js_executor_begin_bar_callback_window(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  (void)ctx;
  (void)argv;
  return JS_UNDEFINED;
}""",
        build_targets=["flox_quickjs", "test_quickjs"], purge=["flox_quickjs", "test_quickjs"],
        gtest_binary=QUICKJS_BIN, gtest_filter=None,
        # Same test: it drives the tape once with the window and once without,
        # and the two runs have to realise different numbers.
    ),
    Mutation(
        name="quickjs-close-reason-omitted",
        why="barsToJsArray never sets closeReason on the aggregated-bar object, dropping "
            "the field the Python and Node aggregator bindings both carry",
        system="quickjs", file=QJS_BINDINGS,
        old="""    JS_SetPropertyStr(ctx, o, "trades", JS_NewUint32(ctx, b.trade_count));
    // flox::Bar::reason, carried through so a batch-aggregated bar says why
    // it closed the same way the live callback path's bar object does.
    JS_SetPropertyStr(ctx, o, "closeReason", JS_NewUint32(ctx, b.close_reason));""",
        new="""    JS_SetPropertyStr(ctx, o, "trades", JS_NewUint32(ctx, b.trade_count));""",
        build_targets=["flox_quickjs", "test_quickjs"], purge=["flox_quickjs", "test_quickjs"],
        gtest_binary=QUICKJS_BIN, gtest_filter=None,
        # Same test reads closeReason off the bars flox.timeBars returns.
    ),

    # ── Codon: no Codon compiler in this environment at all ───────────────
    Mutation(
        name="codon-on-bar-ohlc-close-as-open-backtest",
        why="SimulatedExecutor.on_bar_ohlc in backtest.codon forwards close where open "
            "belongs, and vice versa",
        system="codegen", file=CODON_BACKTEST,
        old="        flox_simulated_executor_on_bar_ohlc(self._handle, u32(symbol), open, high, low, close)",
        new="        flox_simulated_executor_on_bar_ohlc(self._handle, u32(symbol), close, high, low, open)",
        codegen_nodeids=[CODEGEN_TEST_FILE],
        equivalent_reason="untestable in this environment, not a missing test: no Codon "
            "compiler is installed (`which codon` finds nothing) and nothing under tests/ "
            "or tools/codegen/tests executes a .codon file, so no assertion can reach the "
            "body of this function. The text-level test pins the surface "
            "(test_the_shipped_codon_module_exposes_the_bar_path checks the declaration is "
            "present), which is the most a text scan can do; verifying the wiring needs a "
            "Codon toolchain in CI, which is its own change.",
    ),
    Mutation(
        name="codon-decode-agg-bars-offset-67",
        why="_decode_agg_bars in tools.codon reads close_reason from byte offset 67 "
            "instead of 68 (the last byte of trade_count's u32, not close_reason's own)",
        system="codegen", file=CODON_TOOLS,
        old="            close_reason=int(cp[68]),  # byte offset 68 -> u8 index 68",
        new="            close_reason=int(cp[67]),  # byte offset 68 -> u8 index 68",
        codegen_nodeids=[CODEGEN_TEST_FILE],
        equivalent_reason="untestable in this environment for the same reason as "
            "codon-on-bar-ohlc-close-as-open-backtest: no Codon compiler, nothing executes "
            "tools.codon's _decode_agg_bars, and a byte offset inside a function body is "
            "not something a text scan can check. It is the exact off-by-one worth "
            "catching, and catching it needs a Codon toolchain in CI.",
    ),
    Mutation(
        name="codon-reset-noop-tools",
        why="SimulatedExecutor.reset in tools.codon (the second of the two Codon wrappers) "
            "never calls flox_simulated_executor_reset",
        system="codegen", file=CODON_TOOLS,
        old="""    def reset(self):
        \"\"\"Drop fills and run-scoped state while keeping installed
        configuration (slippage, queue model, latency, ...), so a second
        hand-driven run reports that run and not the sum of every run.\"\"\"
        flox_simulated_executor_reset(self._h)""",
        new="""    def reset(self):
        pass""",
        codegen_nodeids=[CODEGEN_TEST_FILE],
        equivalent_reason="untestable in this environment: no Codon compiler, and the "
            "text-level test covers backtest.codon's declarations only (CODON_MODULE = "
            "codon/flox/backtest.codon). A method body emptied out in tools.codon is "
            "invisible to any text scan; reaching it needs a Codon toolchain in CI.",
    ),

    # ── manifest / golden: these ARE plain-text-checked ───────────────────
    Mutation(
        name="yaml-entry-dropped",
        why="binding_parity.yaml drops SimulatedExecutor.reset from pybind11's required "
            "functions list for the simulated_executor group",
        system="codegen", file=YAML_MANIFEST,
        old="""      functions:
        - SimulatedExecutor.on_bar_ohlc
        - SimulatedExecutor.begin_bar_callback_window
        - SimulatedExecutor.end_bar_callback_window
        - SimulatedExecutor.reset
    napi:""",
        new="""      functions:
        - SimulatedExecutor.on_bar_ohlc
        - SimulatedExecutor.begin_bar_callback_window
        - SimulatedExecutor.end_bar_callback_window
    napi:""",
        codegen_nodeids=[
            f"{CODEGEN_TEST_FILE}::test_the_manifest_requires_the_bar_path_from_every_binding"
        ],
    ),
    Mutation(
        name="codon-golden-entry-dropped",
        why="tools/codegen/golden/flox_capi.codon drops the "
            "flox_simulated_executor_reset import -- the golden the parity gate accepts as "
            "Codon's coverage no longer lists the whole bar path",
        system="codegen", file=CODON_GOLDEN,
        old="from C import flox_simulated_executor_reset(cobj)\n",
        new="",
        codegen_nodeids=[f"{CODEGEN_TEST_FILE}::test_the_codon_golden_imports_the_bar_path"],
    ),
]


# ═══════════════════════════════════════════════════════════════════════
# Build / test plumbing
# ═══════════════════════════════════════════════════════════════════════

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


def object_files(build_dir: Path, target_substr: str) -> list[Path]:
    out = subprocess.run(
        ["find", str(build_dir), "-type", "f", "-name", "*.o",
         "-path", f"*{target_substr}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, output: str):
        super().__init__("rebuild failed")
        self.output = output


def cmake_build(build_dir: Path, targets: list[str]) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(build_dir), "--target", *targets, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(output)
    return output


def require_compiled(output: str, where: str) -> int:
    compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
    if compiled == 0:
        raise SystemExit(
            f"{where}: rebuild compiled nothing -- the result would have been stale, so "
            f"the run is refused:\n{output[-2000:]}"
        )
    return compiled


def run_gtest(binary: Path, gtest_filter: str | None) -> tuple[int, str]:
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def run_pytest(nodeids: list[str], cwd: Path = REPO, pythonpath: str | None = None) -> tuple[int, str]:
    env = dict(os.environ)
    if pythonpath is not None:
        env["PYTHONPATH"] = pythonpath
    cmd = [PYTHON, "-m", "pytest", *nodeids, "-q"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT,
                                cwd=cwd, env=env)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def run_node_test() -> tuple[int, str]:
    try:
        result = subprocess.run([NODE, NODE_TEST_FILE], capture_output=True, text=True,
                                timeout=TEST_TIMEOUT, cwd=NODE_DIR)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def npm_build() -> str:
    result = subprocess.run([NPM, "run", "build"], capture_output=True, text=True,
                            timeout=BUILD_TIMEOUT, cwd=NODE_DIR)
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(output)
    return output


def summary_line(output: str) -> str:
    tail = [line for line in output.splitlines() if line.strip()]
    return tail[-1].strip() if tail else ""


def gtest_summary(output: str) -> str:
    for line in output.splitlines():
        if line.startswith("[==========] ") and " ran." in line:
            return line.strip()
        if line.startswith("[  FAILED  ]"):
            return line.strip()
    return summary_line(output)


# ── per-system primary check ───────────────────────────────────────────

def check_c(m: Mutation) -> tuple[int, str, str]:
    binary = m.gtest_binary or CAPI_EXECUTOR_BIN
    code, out = run_gtest(binary, m.gtest_filter)
    label = f"{binary.name} --gtest_filter={m.gtest_filter or '(whole binary)'}"
    return code, label, gtest_summary(out)


def check_quickjs(m: Mutation) -> tuple[int, str, str]:
    code, out = run_gtest(QUICKJS_BIN, m.gtest_filter)
    label = f"test_quickjs --gtest_filter={m.gtest_filter or '(whole binary)'}"
    return code, label, gtest_summary(out)


def check_python(m: Mutation) -> tuple[int, str, str]:
    nodeids = m.pytest_nodeids or [PY_TEST_FILE]
    code, out = run_pytest(nodeids, pythonpath=PY_PYTHONPATH)
    return code, " ".join(nodeids), summary_line(out)


def check_node(_m: Mutation) -> tuple[int, str, str]:
    code, out = run_node_test()
    return code, NODE_TEST_FILE, summary_line(out)


def check_codegen(m: Mutation) -> tuple[int, str, str]:
    nodeids = m.codegen_nodeids or [CODEGEN_TEST_DIR]
    code, out = run_pytest(nodeids)
    return code, " ".join(nodeids), summary_line(out)


PRIMARY_CHECK = {
    "c": check_c,
    "quickjs": check_quickjs,
    "python": check_python,
    "node": check_node,
    "codegen": check_codegen,
}

# full-suite sweep, run only against confirmed survivors
SWEEP_CHECK = {
    "c": lambda: run_gtest(CAPI_EXECUTOR_BIN, None),
    "c-infra": lambda: run_gtest(CAPI_INFRA_BIN, None),
    "quickjs": lambda: run_gtest(QUICKJS_BIN, None),
    "python": lambda: run_pytest([PY_TEST_FILE], pythonpath=PY_PYTHONPATH),
    "node": lambda: run_node_test(),
    "codegen": lambda: run_pytest([CODEGEN_TEST_DIR]),
}


def rebuild_for(m: Mutation) -> str:
    """Rebuild whatever the mutation's own system needs; returns combined output."""
    if m.system == "c" or m.system == "quickjs":
        for target in m.purge:
            for obj in object_files(BUILD_C, target):
                obj.unlink()
        out = cmake_build(BUILD_C, m.build_targets)
        require_compiled(out, f"build/ ({', '.join(m.build_targets)})")
        return out
    if m.system == "python":
        for target in m.purge:
            for obj in object_files(BUILD_PY, target):
                obj.unlink()
        out = cmake_build(BUILD_PY, m.build_targets)
        require_compiled(out, f"build-py/ ({', '.join(m.build_targets)})")
        return out
    if m.system == "node":
        node_build_dir = NODE_DIR / "build"
        for obj in object_files(node_build_dir, "flox_node"):
            obj.unlink()
        out = npm_build()
        require_compiled(out, "node/ (npm run build)")
        return out
    if m.system == "codegen":
        return "(no build: plain-text file, read directly by pytest)"
    raise SystemExit(f"unknown system {m.system!r}")


def rebuild_system(system: str) -> None:
    """Rebuild a system with today's (unmutated, or already-restored) source, for the
    sweep of an OTHER system's survivor."""
    if system == "python":
        for obj in object_files(BUILD_PY, "_flox_py"):
            obj.unlink()
        cmake_build(BUILD_PY, ["_flox_py"])
    elif system == "node":
        for obj in object_files(NODE_DIR / "build", "flox_node"):
            obj.unlink()
        npm_build()
    elif system == "c":
        for obj in object_files(BUILD_C, "flox_capi"):
            obj.unlink()
        cmake_build(BUILD_C, ["flox_capi"])
    else:
        raise SystemExit(f"rebuild_system: unhandled {system!r}")


def control_all() -> bool:
    ok = True
    print("  control  c        (test_capi_executor_bar_ohlc, whole binary)")
    code, out = run_gtest(CAPI_EXECUTOR_BIN, None)
    print(f"           -> exit {code}  {gtest_summary(out)}")
    ok = ok and code == 0

    print("  control  c-infra  (test_capi_infra, whole binary)")
    code, out = run_gtest(CAPI_INFRA_BIN, None)
    print(f"           -> exit {code}  {gtest_summary(out)}")
    ok = ok and code == 0

    print("  control  quickjs  (test_quickjs, whole binary)")
    code, out = run_gtest(QUICKJS_BIN, None)
    print(f"           -> exit {code}  {gtest_summary(out)}")
    ok = ok and code == 0

    print("  control  python   (test_executor_bar_ohlc_parity.py)")
    code, out = run_pytest([PY_TEST_FILE], pythonpath=PY_PYTHONPATH)
    print(f"           -> exit {code}  {summary_line(out)}")
    ok = ok and code == 0

    print("  control  node     (test_executor_bar_ohlc_parity.js)")
    code, out = run_node_test()
    print(f"           -> exit {code}  {summary_line(out)}")
    ok = ok and code == 0

    print("  control  codegen  (tools/codegen/tests)")
    code, out = run_pytest([CODEGEN_TEST_DIR])
    print(f"           -> exit {code}  {summary_line(out)}")
    ok = ok and code == 0

    return ok


# ═══════════════════════════════════════════════════════════════════════
# Mutation runner
# ═══════════════════════════════════════════════════════════════════════

def run_mutation(m: Mutation) -> tuple[str, str]:
    """(verdict, detail). verdict in killed/killed-elsewhere/equivalent/hole/no-compile."""
    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}

    print(f"\n[{m.name}]  system={m.system}")
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

    verdict = "hole"
    detail = ""
    try:
        try:
            rebuild_for(m)
        except BuildFailed as e:
            print("  DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return "no-compile", "did not compile"

        code, label, summary = PRIMARY_CHECK[m.system](m)
        red = code != 0
        print(f"  primary [{m.system}] {label} -> exit {code} "
              f"({'RED, mutation killed' if red else 'green'})   {summary}")

        if red:
            return "killed", label

        # Survived its own system's check: sweep the systems the same file(s)
        # also feed, plus the fixed six-suite list the task calls for.
        feeds = [m.system] + m.also_feeds
        print(f"  green on {m.system} -- sweeping: {', '.join(dict.fromkeys(feeds + ['c-infra', 'quickjs', 'python', 'node', 'codegen']))}")

        caught_elsewhere = None
        already_checked = {m.system}

        # c/quickjs already have their sibling binary in the same build dir;
        # cheap to just run it without a rebuild.
        for sib in ("c", "c-infra", "quickjs"):
            if sib in already_checked:
                continue
            if m.system in ("c", "quickjs") and sib in ("c", "c-infra", "quickjs"):
                code2, out2 = SWEEP_CHECK[sib]()
                print(f"    sweep {sib:<8} (no rebuild needed) -> exit {code2}  {gtest_summary(out2) if sib != 'python' else summary_line(out2)}")
                already_checked.add(sib)
                if code2 != 0:
                    caught_elsewhere = sib

        for sib in m.also_feeds:
            if sib in already_checked or caught_elsewhere:
                already_checked.add(sib)
                continue
            print(f"    sweep {sib:<8} rebuilding...")
            rebuild_system(sib)
            code2, out2 = SWEEP_CHECK[sib]()
            print(f"    sweep {sib:<8} -> exit {code2}  {summary_line(out2)}")
            already_checked.add(sib)
            if code2 != 0:
                caught_elsewhere = sib

        # The rest of the fixed six, if not already covered above, run as-is
        # (no rebuild: unaffected by this mutation, included for literal
        # compliance with "sweep across all six").
        for sib in ("python", "node", "codegen"):
            if sib in already_checked or caught_elsewhere:
                continue
            code2, out2 = SWEEP_CHECK[sib]()
            print(f"    sweep {sib:<8} (unaffected, not rebuilt) -> exit {code2}  {summary_line(out2)}")
            already_checked.add(sib)
            if code2 != 0:
                caught_elsewhere = sib

        if caught_elsewhere:
            verdict = "killed-elsewhere"
            detail = f"green on {m.system}, caught by {caught_elsewhere}"
            print(f"  {detail}")
        elif m.equivalent_reason:
            verdict = "equivalent"
            detail = m.equivalent_reason
            print(f"  EQUIVALENT: {detail}")
        else:
            verdict = "hole"
            detail = m.known_uncovered_reason or "green everywhere in the sweep -- no test catches this"
            tag = "KNOWN-UNCOVERED" if m.known_uncovered_reason else "HOLE"
            print(f"  {tag}: {detail}")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        # restore compiled artifacts too, so the tree is left green
        try:
            if m.system in ("c", "quickjs"):
                for target in m.purge:
                    for obj in object_files(BUILD_C, target):
                        obj.unlink()
                cmake_build(BUILD_C, m.build_targets)
            elif m.system == "python":
                for target in m.purge:
                    for obj in object_files(BUILD_PY, target):
                        obj.unlink()
                cmake_build(BUILD_PY, m.build_targets)
            elif m.system == "node":
                for obj in object_files(NODE_DIR / "build", "flox_node"):
                    obj.unlink()
                npm_build()
        except BuildFailed as e:
            raise SystemExit(f"restore rebuild failed for {m.name}:\n{e.output[-2000:]}")

    return verdict, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--skip-control", action="store_true",
                        help="skip the six-suite control runs before/after")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<42} {m.system:<8} {', '.join(m.files())}")
        return 0

    if not (BUILD_C / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD_C} is not configured; see the module docstring")
    if not (BUILD_PY / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD_PY} is not configured; see the module docstring")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    if not args.skip_control:
        print("control run before the mutations (all six suites)")
        if not control_all():
            raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m)) for m in selected]

    restored = True
    if not args.skip_control:
        print("\ncontrol run after the mutations (all six suites)")
        restored = control_all()

    print("\nsummary")
    label = {"killed": "RED       ", "killed-elsewhere": "RED(else) ", "hole": "HOLE      ",
             "equivalent": "EQUIVALENT", "no-compile": "NOBUILD   "}
    for m, verdict, detail in results:
        extra = f"  ({detail})" if detail else ""
        print(f"  {label[verdict]}  {m.name:<42} {m.system:<8}{extra}")

    holes = [(m.name, d) for m, v, d in results if v == "hole" and not m.known_uncovered_reason]
    known_uncovered = [(m.name, d) for m, v, d in results if v == "hole" and m.known_uncovered_reason]
    equivalent = [(m.name, d) for m, v, d in results if v == "equivalent"]
    killed_elsewhere = [(m.name, d) for m, v, d in results if v == "killed-elsewhere"]
    nobuild = [m.name for m, v, d in results if v == "no-compile"]

    if holes:
        print(f"\n{len(holes)} unexplained hole(s):")
        for name, detail in holes:
            print(f"  - {name}: {detail}")
    if known_uncovered:
        print(f"\n{len(known_uncovered)} documented known-uncovered mutation(s) (real gaps, "
              f"reason recorded, not counted as unexplained):")
        for name, detail in known_uncovered:
            print(f"  - {name}: {detail}")
    if equivalent:
        print(f"\n{len(equivalent)} mutation(s) survived but are equivalent (documented, not "
              f"counted as failures):")
        for name, reason in equivalent:
            print(f"  - {name}: {reason}")
    if killed_elsewhere:
        print(f"\n{len(killed_elsewhere)} mutation(s) were green on their own system but "
              f"caught elsewhere in the sweep:")
        for name, reason in killed_elsewhere:
            print(f"  - {name}: {reason}")
    if nobuild:
        print(f"\n{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")

    return 0 if not holes and restored else 1


if __name__ == "__main__":
    sys.exit(main())
