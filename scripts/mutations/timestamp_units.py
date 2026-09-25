#!/usr/bin/env python3
"""Mutation harness for the QuickJS/Python timestamp-unit fixes (ts-units-code).

The fix branch touches five things: js_load_csv now emits BigInt
nanoseconds; the Engine prelude in js_strategy.cpp normalises every bar/
signal ts to BigInt ns before comparing, sorting or feeding the executor;
flox.timeBars/heikinBars convert their nanosecond interval to the seconds
the C ABI wants, with a nudge so the round trip never truncates a
nanosecond short; toBookSnapshot and flox_get_symbol_context fill
bid_qty_raw/ask_qty_raw from bidAtPrice/askAtPrice instead of hardcoding 0;
Engine.ts() returns int64 instead of float64; and docs/bindings/README.md
carries a real per-binding capability table. This script breaks each piece
one at a time, in the source, and checks that the tests written for it
(tests/test_quickjs.cpp, tests/test_capi_book_snapshot.cpp,
python/tests/test_engine_ts_dtype.py,
python/tests/test_binding_docs_contract.py) go red.

Three build/test surfaces, chosen per mutation:

  cpp     rebuild a tests/*.cpp gtest binary out of build/, run it with a
          --gtest_filter (or the whole binary when no acceptance test
          names the case).
  python  rebuild the _flox_py extension out of build-py/, run pytest
          against python/tests/test_engine_ts_dtype.py node ids.
  text    no C++ compiles at all: the mutated file is prose or a comment
          block, and the check is a pytest run against
          python/tests/test_binding_docs_contract.py, which reads the
          file's text directly.

A few mutations (kind pyi / kind js) touch files nothing in the given test
surfaces reads at all -- python/flox_py/_flox_py/__init__.pyi is never
imported, quickjs/examples/backtest_sma.js has no assertions of its own.
Those are run anyway, with no primary check, straight into the sweep
below; if the sweep does not catch them either they are reported as
documented holes.

A mutation that stays green on its primary check is not yet "equivalent"
or "alive": the SURVIVORS (kind cpp/python/text; kind pyi/js always go
through it) are re-run against every other surface -- the other two cpp
gtest binaries, both python test files, and all 14 quickjs/examples
scripts through flox_js_runner -- in case something elsewhere happens to
notice. Only if nothing anywhere notices is a mutation "alive": a real
hole, unless its `equivalent_reason` says why the change has no observable
effect at all.

Every run is honest about the build: the mutated file's hash is printed
before and after, the primary target's own object files are deleted so
nothing is served from cache, a cpp/python rebuild's output has to contain
"Building CXX" or the run is refused, and every subprocess runs under a
timeout. A mutation that does not compile is not a mutation.

Usage:

    python3 scripts/mutations/timestamp_units.py             # control, all, control
    python3 scripts/mutations/timestamp_units.py --list
    python3 scripts/mutations/timestamp_units.py --only loadcsv-back-to-ms-number
    python3 scripts/mutations/timestamp_units.py --skip-sweep   # primary check only

Build directories are expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON \\
          -DFLOX_BUILD_CAPI=ON -DFLOX_BUILD_QUICKJS=ON -DFLOX_NATIVE=OFF
    cmake -S . -B build-py -DCMAKE_BUILD_TYPE=Release -DFLOX_BUILD_PYTHON=ON \\
          -DFLOX_ENABLE_BACKTEST=ON -DPYTHON_EXECUTABLE=<venv>/bin/python3.12 \\
          -Dpybind11_DIR=<venv .../pybind11/share/cmake/pybind11>
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
BUILD_PY = REPO / "build-py"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 480
TEST_TIMEOUT = 120
PYTEST_TIMEOUT = 120
EXAMPLES_TIMEOUT = 60

_venv_python = REPO / ".venv" / "bin" / "python3.12"
PYTHON = str(_venv_python) if _venv_python.is_file() else sys.executable
PYTHONPATH = str(BUILD_PY / "python")

CPP_TARGETS = ["test_quickjs", "test_capi_book_snapshot", "test_capi_infra"]
CPP_BINARY = {t: BUILD / "tests" / t for t in CPP_TARGETS}
JS_RUNNER_TARGET = "flox_js_runner"
JS_RUNNER_BINARY = BUILD / "src" / "quickjs" / JS_RUNNER_TARGET
EXAMPLES_DIR = REPO / "quickjs" / "examples"

PY_TEST_FILES = [
    "python/tests/test_engine_ts_dtype.py",
    "python/tests/test_binding_docs_contract.py",
]

JS_BINDINGS = "src/quickjs/js_bindings.cpp"
JS_STRATEGY = "src/quickjs/js_strategy.cpp"
BRIDGE_STRATEGY = "include/flox/capi/bridge_strategy.h"
FLOX_CAPI_CPP = "src/capi/flox_capi.cpp"
FLOX_CAPI_SPEC = "include/flox/capi/flox_capi_spec.hpp"
FLOX_PY_CPP = "python/flox_py.cpp"
FLOX_PY_PYI = "python/flox_py/_flox_py/__init__.pyi"
BINDINGS_README = "docs/bindings/README.md"
QUICKJS_TOOLS_MD = "docs/reference/quickjs/tools.md"
FLOX_D_TS = "quickjs/types/flox.d.ts"
BACKTEST_SMA_JS = "quickjs/examples/backtest_sma.js"


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
    kind: str  # "cpp" | "python" | "text" | "pyi" | "js"
    file: str = ""
    old: str = ""
    new: str = ""
    edits: list[Edit] = field(default_factory=list)
    occurrence: int = 1
    expected_occurrences: int = 1
    # cpp: which gtest binary to rebuild+run as the primary check, and
    # which --gtest_filter (colon-separated for several tests, None for
    # the whole binary).
    cpp_target: str = ""
    gtest_filter: str | None = None
    # python / text: pytest node ids for the primary check. Empty for
    # kind pyi/js, which have no primary check at all -- straight to the
    # sweep.
    pytest_ids: list[str] = field(default_factory=list)
    # An "equivalent" mutation is expected to survive for a documented
    # reason that is not a hole in the tests (the change has no observable
    # effect through any path this harness can exercise). It still runs
    # and still goes through the sweep; it just is not reported as a
    # failure.
    equivalent_reason: str = ""

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
    # ── js_load_csv: BigInt ns reverted / mis-scaled ────────────────────────
    Mutation(
        name="loadcsv-back-to-ms-number",
        why="js_load_csv computes a millisecond value alongside the nanosecond one, the first "
            "half of the pre-fix pattern -- but by itself this only adds an unused local "
            "(ts_ms is never read anywhere below), so nothing observable changes yet; the "
            "property write is still JS_NewBigInt64(ts_ns). Confirmed equivalent, not a hole: "
            "see loadcsv-back-to-ms-number-wire, which changes the actual JS_SetPropertyStr "
            "call to use it and is killed",
        kind="cpp",
        file=JS_BINDINGS,
        old="""      int64_t ts_ns = detectTimestampNs(std::stoll(parts[0]));
      double o = std::stod(parts[1]);""",
        new="""      int64_t ts_ns = detectTimestampNs(std::stoll(parts[0]));
      int64_t ts_ms = ts_ns / 1'000'000LL;
      double o = std::stod(parts[1]);""",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.LoadCsvBarTimestampIsBigIntNanoseconds:"
                     "JsBarTimestampUnits.CsvAndAggregatorBarsMixWithoutTypeError",
        equivalent_reason="adds an unused local (ts_ms computed, never read); the property "
            "write two lines below is untouched by this edit alone, so no test can observe a "
            "difference. The real revert is loadcsv-back-to-ms-number-wire, which mutates the "
            "JS_SetPropertyStr call itself and is killed by the same two tests.",
    ),
    Mutation(
        name="loadcsv-back-to-ms-number-wire",
        why="the ts property itself is set from ts_ms via JS_NewInt64 -- the second half of the "
            "revert above (a Number, not a BigInt)",
        kind="cpp",
        file=JS_BINDINGS,
        old='JS_SetPropertyStr(c, o2, "ts", JS_NewBigInt64(c, ts_ns));',
        new='JS_SetPropertyStr(c, o2, "ts", JS_NewInt64(c, ts_ns / 1000000));',
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.LoadCsvBarTimestampIsBigIntNanoseconds:"
                     "JsBarTimestampUnits.CsvAndAggregatorBarsMixWithoutTypeError",
    ),
    Mutation(
        name="loadcsv-bigint-divided-by-1e6",
        why="ts stays a BigInt (typeof check still passes) but is divided by 1e6 first -- a "
            "millisecond value wearing a nanosecond type. tsStr/gapStr, and the exact-nanosecond "
            "subtraction against an aggregator bar, both catch a wrong magnitude even though the "
            "type is right",
        kind="cpp",
        file=JS_BINDINGS,
        old='JS_SetPropertyStr(c, o2, "ts", JS_NewBigInt64(c, ts_ns));',
        new='JS_SetPropertyStr(c, o2, "ts", JS_NewBigInt64(c, ts_ns / 1000000));',
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.LoadCsvBarTimestampIsBigIntNanoseconds:"
                     "JsBarTimestampUnits.CsvAndAggregatorBarsMixWithoutTypeError",
    ),
    Mutation(
        name="detect-ts-already-ns-bucket-divided",
        why="detectTimestampNs's terminal branch (\"already ns\", the bucket every CSV "
            "timestamp in these tests actually falls into -- 19 digits, >= 1e18) divides by "
            "1000 instead of returning the value unchanged: a column-detection off-by-one-power "
            "at the top of the ladder instead of the bottom",
        kind="cpp",
        file=JS_BINDINGS,
        old="  return ts;  // already ns\n}",
        new="  return ts / 1'000LL;  // already ns\n}",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.LoadCsvBarTimestampIsBigIntNanoseconds:"
                     "JsBarTimestampUnits.CsvAndAggregatorBarsMixWithoutTypeError",
    ),
    Mutation(
        name="adv-detect-ts-seconds-and-us-multipliers-swapped",
        why="the seconds-to-ns and microseconds-to-ns multipliers are swapped (x1000 where the "
            "comment says seconds, x1e9 where it says microseconds) -- a real column-detection "
            "bug, but every CSV timestamp exercised by the suite is a 19-digit already-ns value "
            "that never reaches either branch, so nothing here can tell. "
            "Closed since by LoadCsvScalesSecondsMillisecondsAndMicrosecondsToNanoseconds, which "
            "loads the same instant written in all four units",
        kind="cpp",
        file=JS_BINDINGS,
        old="""  if (ts < 1'000'000'000'000LL)
  {
    return ts * 1'000'000'000LL;  // seconds → ns
  }""",
        new="""  if (ts < 1'000'000'000'000LL)
  {
    return ts * 1'000LL;  // seconds → ns
  }""",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.LoadCsvBarTimestampIsBigIntNanoseconds:"
                     "JsBarTimestampUnits.CsvAndAggregatorBarsMixWithoutTypeError:"
                     "JsBarTimestampUnits.LoadCsvScalesSecondsMillisecondsAndMicrosecondsToNanoseconds",
    ),

    # ── js_strategy.cpp prelude: SignalBuilder / Engine.run ─────────────────
    Mutation(
        name="signalbuilder-not-normalising",
        why="SignalBuilder._add stores the ts argument as-is instead of routing it through "
            "__floxToNs -- a plain Number handed to signals.buy()/sell() would stay a Number and "
            "later break the BigInt comparisons downstream. No acceptance test calls buy/sell "
            "with anything but a bar's own .ts, which is already a BigInt by the time it gets "
            "there (loadCsv emits BigInt), so this identity-vs-conversion difference has nothing "
            "in the suite that can observe it. "
            "Closed since by SignalBuilderNormalisesEveryTimestampToNanosecondBigInt, which hands "
            "buy/sell/limitBuy plain Numbers and reads the type back off sorted()",
        kind="cpp",
        file=JS_STRATEGY,
        old='this._entries.push({ tsNs: __floxToNs(ts), side, qty, price: price || 0, orderType: orderType || 0, symbol: symbol || "" });',
        new='this._entries.push({ tsNs: ts, side, qty, price: price || 0, orderType: orderType || 0, symbol: symbol || "" });',
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.EngineRunAcceptsSignalsTimestampedFromBars:"
                     "JsBarTimestampUnits.SignalBuilderNormalisesEveryTimestampToNanosecondBigInt",
    ),
    Mutation(
        name="comparator-returns-bigint",
        why="__floxCmpNs goes back to a plain subtraction (a < b ? -1 : ...  replaced by a - b), "
            "which is a BigInt when both operands are BigInt. Array.prototype.sort's stable "
            "ordering means a comparator that effectively answers \"equal\" for every pair still "
            "leaves an already-sorted array looking sorted -- and the only data this suite feeds "
            "through sorted()/merged.sort() (one CSV, one symbol, rows already in order; two "
            "signals inserted buy-then-sell in timestamp order) is already sorted before the "
            "comparator runs a single time",
        kind="cpp",
        file=JS_STRATEGY,
        old="function __floxCmpNs(a, b) { return a < b ? -1 : (a > b ? 1 : 0); }",
        new="function __floxCmpNs(a, b) { return a - b; }",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.EngineRunAcceptsSignalsTimestampedFromBars",
    ),
    Mutation(
        name="merged-timeline-divided-to-ms",
        why="Engine.run's merged bar timeline divides each bar's ns BigInt down to ms before "
            "storing it as tsNs -- the field name still says ns, the value does not. "
            "advanceClock and the signal-submission comparison downstream both run on the wrong "
            "scale. Confirmed, not just theorized, to survive: Engine.run has an unconditional "
            "trailing flush after the merged-bar loop (`while (sigIdx < sorted.length) { "
            "...submitOrder... }`) that submits any signal the main loop never reached, at "
            "whatever price/state the executor is in by then. With only two signals against "
            "two bars, the mistimed signal lands there instead and still closes the same round "
            "trip, so totalTrades/finalCapital come back unchanged -- a longer backtest with "
            "bars after the mistimed signal would show the difference; this one cannot. "
            "Closed since by the five-bar fixture in "
            "EngineRunAppliesASignalOnTheBarItIsTimestampedOn, which reads the fill prices and "
            "the executor clock straight off stats",
        kind="cpp",
        file=JS_STRATEGY,
        old="merged.push({ tsNs: __floxToNs(bars[j].ts), key: key, bar: bars[j] });",
        new="merged.push({ tsNs: __floxToNs(bars[j].ts) / 1000000n, key: key, bar: bars[j] });",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.EngineRunAcceptsSignalsTimestampedFromBars:"
                     "JsBarTimestampUnits.EngineRunAppliesASignalOnTheBarItIsTimestampedOn:"
                     "JsBarTimestampUnits.EngineRunHoldsASignalOneNanosecondPastABarUntilTheNextBar",
    ),
    Mutation(
        name="advanceclock-fed-ms",
        why="executor.advanceClock is fed ref.tsNs divided down to ms -- the executor's clock "
            "runs three orders of magnitude behind the bars it is filling against. Survives for "
            "the same confirmed reason as merged-timeline-divided-to-ms: the trailing "
            "unconditional flush loop in Engine.run submits whatever this mutation kept out of "
            "the main loop, so the two-signal/two-bar acceptance case still closes one trade. "
            "Closed since: stats.startTimeNs/endTimeNs report the executor clock at the first and "
            "last fill, and EngineRunAppliesASignalOnTheBarItIsTimestampedOn pins both to the "
            "bar's own nanosecond timestamp",
        kind="cpp",
        file=JS_STRATEGY,
        old="executor.advanceClock(ref.tsNs);",
        new="executor.advanceClock(ref.tsNs / 1000000n);",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.EngineRunAcceptsSignalsTimestampedFromBars:"
                     "JsBarTimestampUnits.EngineRunAppliesASignalOnTheBarItIsTimestampedOn:"
                     "JsBarTimestampUnits.EngineRunHoldsASignalOneNanosecondPastABarUntilTheNextBar",
    ),
    Mutation(
        name="adv-engine-run-signal-boundary-strict-less-than",
        why="the signal-submission comparison drops from <= to < -- a signal timestamped exactly "
            "on a bar (the only case this suite builds: signals.buy(bars[0].ts, ...)) is no "
            "longer submitted at that bar, only at a strictly later one. With only two bars, the "
            "sell signal (timestamped on the last bar) never gets a later bar to submit at in "
            "the main loop -- but Engine.run's trailing unconditional flush "
            "(`while (sigIdx < sorted.length) { ...submitOrder... }`) submits it anyway right "
            "after, and the round trip still closes. "
            "Closed since by the pair EngineRunAppliesASignalOnTheBarItIsTimestampedOn (a signal "
            "exactly on a bar is applied on that bar) and "
            "EngineRunHoldsASignalOneNanosecondPastABarUntilTheNextBar (one nanosecond later is "
            "not)",
        kind="cpp",
        file=JS_STRATEGY,
        old="while (sigIdx < sorted.length && sorted[sigIdx].tsNs <= ref.tsNs) {",
        new="while (sigIdx < sorted.length && sorted[sigIdx].tsNs < ref.tsNs) {",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.EngineRunAcceptsSignalsTimestampedFromBars:"
                     "JsBarTimestampUnits.EngineRunAppliesASignalOnTheBarItIsTimestampedOn:"
                     "JsBarTimestampUnits.EngineRunHoldsASignalOneNanosecondPastABarUntilTheNextBar",
    ),

    # ── flox.timeBars / flox.heikinBars: the ns→s boundary conversion ───────
    Mutation(
        name="timebars-interval-not-converted",
        why="js_agg_time hands the raw nanosecond argument straight to flox_aggregate_time_bars "
            "as if it were already seconds -- the exact pre-fix bug: a one-minute interval asks "
            "for 60 billion seconds and closes no bars at all",
        kind="cpp",
        file=JS_BINDINGS,
        old="return doAgg(c, a, flox_aggregate_time_bars, intervalNsToSeconds(toInt64(c, a[4])));",
        new="return doAgg(c, a, flox_aggregate_time_bars, toDouble(c, a[4]));",
        cpp_target="test_quickjs",
        gtest_filter="JsBarAggregatorUnits.TimeBarsIntervalIsNanoseconds",
    ),
    Mutation(
        name="timebars-interval-converted-to-microseconds",
        why="js_agg_time converts using 1e6 instead of 1e9 -- a plausible off-by-one-thousand "
            "that still \"converts\", just to the wrong unit (microseconds, not seconds)",
        kind="cpp",
        file=JS_BINDINGS,
        old="return doAgg(c, a, flox_aggregate_time_bars, intervalNsToSeconds(toInt64(c, a[4])));",
        new="return doAgg(c, a, flox_aggregate_time_bars, "
            "static_cast<double>(toInt64(c, a[4])) / 1'000'000.0);",
        cpp_target="test_quickjs",
        gtest_filter="JsBarAggregatorUnits.TimeBarsIntervalIsNanoseconds",
    ),
    Mutation(
        name="nudge-removed",
        why="intervalNsToSeconds drops the nextafter loop and returns the plain division -- the "
            "truncation-recovery step the fix comment says exists because ns/1e9 can land one "
            "nanosecond short of the value asked for. The acceptance test's interval, 60 billion "
            "ns (a round minute), divides to exactly 60.0 in double arithmetic with no rounding "
            "error at all, so the nudge loop never actually iterates for it -- nothing in the "
            "suite uses an interval whose round trip needs the nudge to land correctly. "
            "Closed since by TimeBarsIntervalSurvivesADivisionThatDoesNotRoundBack, whose "
            "interval of 1'000'000'007 ns truncates back to 1'000'000'006 without the nudge",
        kind="cpp",
        file=JS_BINDINGS,
        old="""static double intervalNsToSeconds(int64_t interval_ns)
{
  double seconds = static_cast<double>(interval_ns) / 1'000'000'000.0;
  for (int i = 0; i < 4 && interval_ns > 0 &&
                  static_cast<int64_t>(seconds * 1'000'000'000.0) < interval_ns;
       ++i)
  {
    seconds = std::nextafter(seconds, std::numeric_limits<double>::infinity());
  }
  return seconds;
}""",
        new="""static double intervalNsToSeconds(int64_t interval_ns)
{
  return static_cast<double>(interval_ns) / 1'000'000'000.0;
}""",
        cpp_target="test_quickjs",
        gtest_filter="JsBarAggregatorUnits.TimeBarsIntervalIsNanoseconds:"
                     "JsBarAggregatorUnits.TimeBarsIntervalSurvivesADivisionThatDoesNotRoundBack",
    ),
    Mutation(
        name="heikinbars-left-unconverted",
        why="js_agg_heikin keeps intervalNsToSeconds for js_agg_time but drops it for its own "
            "call, going back to toDouble(a[4]) straight through -- heikinBars alone regresses "
            "while timeBars stays fixed",
        kind="cpp",
        file=JS_BINDINGS,
        old="return doAgg(c, a, flox_aggregate_heikin_ashi_bars, intervalNsToSeconds(toInt64(c, a[4])));",
        new="return doAgg(c, a, flox_aggregate_heikin_ashi_bars, toDouble(c, a[4]));",
        cpp_target="test_quickjs",
        gtest_filter="JsBarAggregatorUnits.TimeBarsIntervalIsNanoseconds",
    ),
    Mutation(
        name="tickbars-wrongly-converted",
        why="js_agg_tick starts running its trade-count argument through intervalNsToSeconds "
            "before truncating back to a uint32 -- tickBars takes a plain trade count, not a "
            "nanosecond interval, so a count of 2 becomes 2/1e9 seconds, truncates to 0, and the "
            "aggregator closes bars on a zero-trade boundary instead of every 2 trades. "
            "Closed since by TickBarsCloseOnTheTradeCountTheyAreGiven, which asserts the bar "
            "count and the trades inside each bar, not only the first bar's timestamp type",
        kind="cpp",
        file=JS_BINDINGS,
        old="""  uint32_t got = flox_aggregate_tick_bars(ts.data(), px.data(), qty.data(), side.data(),
                                          n, toUint32(c, a[4]), bars.data(), n);""",
        new="""  uint32_t got = flox_aggregate_tick_bars(
      ts.data(), px.data(), qty.data(), side.data(), n,
      static_cast<uint32_t>(intervalNsToSeconds(toInt64(c, a[4]))), bars.data(), n);""",
        cpp_target="test_quickjs",
        gtest_filter="JsBarTimestampUnits.AggregatorBarTimestampIsBigIntNanoseconds:"
                     "JsBarTimestampUnits.CsvAndAggregatorBarsMixWithoutTypeError:"
                     "JsBarAggregatorUnits.TickBarsCloseOnTheTradeCountTheyAreGiven",
    ),

    # ── FloxBookSnapshot sizes: bridge_strategy.h / flox_capi.cpp ───────────
    Mutation(
        name="bid-qty-from-askatprice",
        why="toBookSnapshot's bid_qty_raw is filled from askAtPrice(*bid) instead of "
            "bidAtPrice(*bid) -- the two best-level sizes end up cross-wired",
        kind="cpp",
        file=BRIDGE_STRATEGY,
        old="snap.bid_qty_raw = c.book.bidAtPrice(*bid).raw();",
        new="snap.bid_qty_raw = c.book.askAtPrice(*bid).raw();",
        cpp_target="test_capi_book_snapshot",
        gtest_filter="CApiBookSnapshot.BestLevelSizesReachTheCConsumer",
    ),
    Mutation(
        name="tobooksnapshot-qty-reverted-to-zero",
        why="toBookSnapshot's whole fix is reverted for the bid side: bid_qty_raw goes back to "
            "the hardcoded 0 under the old \"not directly available\" comment",
        kind="cpp",
        file=BRIDGE_STRATEGY,
        old="snap.bid_qty_raw = c.book.bidAtPrice(*bid).raw();",
        new="snap.bid_qty_raw = 0;  // not directly available from bestBid()",
        cpp_target="test_capi_book_snapshot",
        gtest_filter="CApiBookSnapshot.BestLevelSizesReachTheCConsumer",
    ),
    Mutation(
        name="sizes-filled-on-empty-side",
        why="an empty ask side gets a nonzero size anyway: the else branch of the (missing) "
            "existence check hands back a hardcoded 1 instead of leaving the zero-initialised "
            "snapshot alone, so a consumer can no longer tell a genuinely empty side from a "
            "sized one",
        kind="cpp",
        file=BRIDGE_STRATEGY,
        old="""    if (ask)
    {
      snap.ask_price_raw = ask->raw();
      snap.ask_qty_raw = c.book.askAtPrice(*ask).raw();
    }""",
        new="""    if (ask)
    {
      snap.ask_price_raw = ask->raw();
      snap.ask_qty_raw = c.book.askAtPrice(*ask).raw();
    }
    else
    {
      snap.ask_qty_raw = 1;  // BUG: an empty side reports a phantom size
    }""",
        cpp_target="test_capi_book_snapshot",
        gtest_filter="CApiBookSnapshot.EmptySideReportsZeroPriceAndZeroSize",
    ),
    Mutation(
        name="symbol-context-qty-reverted-to-zero",
        why="flox_get_symbol_context's half of the same fix is reverted: bid_qty_raw/ask_qty_raw "
            "go back to hardcoded 0. Nothing in tests/, python/tests/ or the QuickJS bindings "
            "calls flox_get_symbol_context at all (grep confirms it: the function is reachable "
            "only through the raw C ABI, which none of the given surfaces exercise) -- this is "
            "the \"toBookSnapshot versus flox_get_symbol_context\" split the task calls out, and "
            "only the toBookSnapshot half has a test",
        kind="cpp",
        file=FLOX_CAPI_CPP,
        old="""  out->book.bid_qty_raw = bid ? c.book.bidAtPrice(*bid).raw() : 0;
  out->book.ask_price_raw = ask ? ask->raw() : 0;
  out->book.ask_qty_raw = ask ? c.book.askAtPrice(*ask).raw() : 0;""",
        new="""  out->book.bid_qty_raw = 0;
  out->book.ask_price_raw = ask ? ask->raw() : 0;
  out->book.ask_qty_raw = 0;""",
        cpp_target="test_capi_book_snapshot",
        gtest_filter=None,  # nothing in this binary calls flox_get_symbol_context either
    ),

    # ── Engine.ts(): int64 reverted / mistyped ──────────────────────────────
    Mutation(
        name="engine-ts-back-to-float64",
        why="Engine::timestamps goes back to py::array_t<double>, rounding every nanosecond "
            "value above 2^53 the same way the original bug did",
        kind="python",
        file=FLOX_PY_CPP,
        old="""  py::array_t<int64_t> timestamps(const std::string& symbol = "") const
  {
    auto& bars = resolve(symbol).bars;
    py::array_t<int64_t> out(bars.size());
    auto* p = out.mutable_data();
    for (size_t i = 0; i < bars.size(); ++i)
    {
      p[i] = bars[i].timestamp_ns;
    }
    return out;
  }""",
        new="""  py::array_t<double> timestamps(const std::string& symbol = "") const
  {
    auto& bars = resolve(symbol).bars;
    py::array_t<double> out(bars.size());
    auto* p = out.mutable_data();
    for (size_t i = 0; i < bars.size(); ++i)
    {
      p[i] = static_cast<double>(bars[i].timestamp_ns);
    }
    return out;
  }""",
        pytest_ids=[
            "python/tests/test_engine_ts_dtype.py::EngineTimestampDtype::test_ts_dtype_is_int64",
            "python/tests/test_engine_ts_dtype.py::EngineTimestampDtype::"
            "test_ts_round_trips_above_two_to_the_53",
        ],
    ),
    Mutation(
        name="pyi-ts-as-float",
        why="the generated stub types Engine.ts() as NDArray[float64] again, contradicting the "
            "int64 the compiled extension actually returns. Nothing in the given test surfaces "
            "imports or type-checks __init__.pyi at all -- only `scripts/gen_pyi_stubs.py "
            "--check` (a separate codegen gate, outside tests/, python/tests/ and the examples "
            "runner) would notice the stub went stale",
        kind="pyi",
        file=FLOX_PY_PYI,
        old="def ts(self, symbol: str = '') -> numpy.typing.NDArray[numpy.int64]:",
        new="def ts(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:",
    ),

    # ── docs/bindings/README.md: the capability table ────────────────────────
    Mutation(
        name="docs-table-row-flipped-to-yes",
        why="the BacktestRunner row's QuickJS cell is flipped from \"no\" to \"yes\" -- exactly "
            "the false claim the fix exists to correct",
        kind="text",
        file=BINDINGS_README,
        old="| `BacktestRunner` — event-driven replay (`run_csv`, `run_tape`, `run_ohlcv`, `run_bars`) | yes | yes | yes | no |",
        new="| `BacktestRunner` — event-driven replay (`run_csv`, `run_tape`, `run_ohlcv`, `run_bars`) | yes | yes | yes | yes |",
        pytest_ids=[
            "python/tests/test_binding_docs_contract.py::BindingCapabilityTable::"
            "test_backtest_runner_is_marked_absent_for_quickjs",
        ],
    ),
    Mutation(
        name="removed-sentence-restored",
        why="the blanket \"The API shape is the same across languages.\" sentence is put back "
            "next to the (still-present) capability table -- the two now directly contradict "
            "each other on the same page",
        kind="text",
        file=BINDINGS_README,
        old="All bindings expose the same core model: strategy callbacks, order emission, position queries. What each one carries around that model differs, and the table below says where.",
        new="All bindings expose the same core model: strategy callbacks, order emission, position queries. The API shape is the same across languages.",
        pytest_ids=[
            "python/tests/test_binding_docs_contract.py::BindingCapabilityTable::"
            "test_blanket_sameness_claim_is_gone",
        ],
    ),
    Mutation(
        name="capi-spec-comment-removed",
        why="bid_qty_raw's explanatory comment is dropped entirely (ask_qty_raw's stays) -- a "
            "reader is back to a bare field with no idea what a 0 means",
        kind="text",
        file=FLOX_CAPI_SPEC,
        old="    int64_t bid_qty_raw;    // size resting at the best bid, 0 when the book cannot say",
        new="    int64_t bid_qty_raw;",
        pytest_ids=[
            "python/tests/test_binding_docs_contract.py::BookSnapshotSizeFieldsAreDocumented::"
            "test_size_fields_carry_a_comment",
        ],
    ),
    Mutation(
        name="capi-spec-cannot-say-removed",
        why="both size fields keep a comment, but the wording that actually explains the 0 "
            "(\"0 when the book cannot say\") is replaced by something that does not say it",
        kind="text",
        edits=[
            Edit(FLOX_CAPI_SPEC,
                 "    int64_t bid_qty_raw;    // size resting at the best bid, 0 when the book cannot say",
                 "    int64_t bid_qty_raw;    // size resting at the best bid"),
            Edit(FLOX_CAPI_SPEC,
                 "    int64_t ask_qty_raw;    // size resting at the best ask, 0 when the book cannot say",
                 "    int64_t ask_qty_raw;    // size resting at the best ask"),
        ],
        pytest_ids=[
            "python/tests/test_binding_docs_contract.py::BookSnapshotSizeFieldsAreDocumented::"
            "test_size_fields_say_what_zero_means",
        ],
    ),

    # ── own reading: pieces the given test surfaces cannot see at all ───────
    Mutation(
        name="adv-flox-d-ts-bar-ts-reverted-to-number",
        why="quickjs/types/flox.d.ts's Bar.ts field goes back to `number` instead of `bigint`, "
            "directly contradicting what js_load_csv/the aggregators actually hand back and what "
            "the fix's own commit updated this file to say. No test in tests/, python/tests/ or "
            "the examples runner type-checks or even reads flox.d.ts -- TypeScript users of the "
            "embedded binding are the only audience for this file, and nothing here represents "
            "them",
        kind="text",
        file=FLOX_D_TS,
        old="    readonly ts: bigint;",
        new="    readonly ts: number;",
    ),
    Mutation(
        name="adv-tools-md-interval-reverted-to-seconds",
        why="docs/reference/quickjs/tools.md's timeBars row is edited to claim the interval is "
            "in seconds, not nanoseconds -- the exact misdocumentation the original bug report "
            "was about, reintroduced in prose. test_binding_docs_contract.py only reads "
            "docs/bindings/README.md and flox_capi_spec.hpp; nothing checks this file",
        kind="text",
        file=QUICKJS_TOOLS_MD,
        old="| `flox.timeBars(ts, px, qty, sides, intervalNs)` | Interval in nanoseconds — `60000000000` is one minute |",
        new="| `flox.timeBars(ts, px, qty, sides, intervalNs)` | Interval in seconds |",
    ),
    Mutation(
        name="adv-backtest-sma-example-ts-divided-to-ms",
        why="the SMA example divides the bar's nanosecond BigInt down before handing it to "
            "SignalBuilder -- a script author copying the old pre-fix pattern. flox_js_runner "
            "does not assert anything about the numbers backtest_sma.js prints, only that it "
            "runs to completion without throwing, and a scaled-down but still-monotonic "
            "timestamp does not throw -- it just backtests against the wrong clock silently. "
            "This is the concrete case for why 'all 14 examples exit 0' is not proof of "
            "numeric correctness",
        kind="js",
        file=BACKTEST_SMA_JS,
        old="  var tsNs = b.ts;",
        new="  var tsNs = b.ts / 1000000n;",
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


def object_files(build_dir: Path, target: str) -> list[Path]:
    out = subprocess.run(
        ["find", str(build_dir), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


def object_files_for_source(build_dir: Path, file_relpath: str) -> list[Path]:
    """The compiled object(s) for the .cpp file itself, wherever the library that
    owns it put them -- js_bindings.cpp/js_strategy.cpp live under
    flox_quickjs.dir, flox_py.cpp under _flox_py.dir, flox_capi.cpp under
    flox_capi.dir, none of which is the top-level test/extension target's own
    .dir. A header (bridge_strategy.h, flox_capi_spec.hpp) has no object of its
    own; every .cpp that includes it is invalidated by cmake's own dependency
    tracking on the header's mtime instead, which primary_check's rebuild
    output ("Building CXX" for that .cpp) confirms actually ran.
    """
    if not file_relpath.endswith(".cpp"):
        return []
    basename = Path(file_relpath).name
    out = subprocess.run(
        ["find", str(build_dir), "-type", "f", "-name", f"{basename}.o"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, output: str):
        super().__init__("rebuild failed")
        self.output = output


def cmake_build(build_dir: Path, target: str, timeout: int = BUILD_TIMEOUT) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(build_dir), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=timeout, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(output)
    return output


def require_compiled(output: str, what: str) -> int:
    compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
    if compiled == 0:
        raise SystemExit(
            f"rebuild of {what} compiled nothing -- the result would have been a stale "
            f"binary, so the run is refused:\n{output[-2000:]}"
        )
    return compiled


def run_gtest(target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = CPP_BINARY[target]
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def run_pytest(node_ids: list[str], timeout: int = PYTEST_TIMEOUT) -> tuple[int, str]:
    env = dict(os.environ)
    env["PYTHONPATH"] = PYTHONPATH
    cmd = [PYTHON, "-m", "pytest", *node_ids, "-q"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO,
                                env=env)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout}s"
    return result.returncode, result.stdout + result.stderr


def run_examples() -> tuple[bool, str]:
    """All 14 quickjs/examples scripts through flox_js_runner. True iff every one exits 0."""
    if not JS_RUNNER_BINARY.is_file():
        return False, f"{JS_RUNNER_BINARY} missing"
    scripts = sorted(EXAMPLES_DIR.glob("*.js"))
    if not scripts:
        return False, "no example scripts found"
    lines = []
    ok = True
    for script in scripts:
        try:
            result = subprocess.run([str(JS_RUNNER_BINARY), str(script)], capture_output=True,
                                    text=True, timeout=EXAMPLES_TIMEOUT, cwd=REPO)
        except subprocess.TimeoutExpired:
            ok = False
            lines.append(f"  {script.name}: timed out")
            continue
        if result.returncode != 0:
            ok = False
            lines.append(f"  {script.name}: exit {result.returncode}\n"
                         f"{(result.stdout + result.stderr)[-500:]}")
        else:
            lines.append(f"  {script.name}: ok")
    return ok, "\n".join(lines)


def summary_line(output: str) -> str:
    tail = [line for line in output.splitlines() if line.strip()]
    return tail[-1].strip() if tail else ""


def control() -> bool:
    """Unmutated tree: every cpp target rebuilds and passes whole, both python test files
    pass, all examples exit 0."""
    ok = True
    for target in CPP_TARGETS:
        for obj in object_files(BUILD, target):
            obj.unlink()
        try:
            cmake_build(BUILD, target)
        except BuildFailed as e:
            print(f"  control {target}: BUILD FAILED\n{e.output[-3000:]}")
            ok = False
            continue
        code, output = run_gtest(target, None)
        state = "green" if code == 0 else "RED"
        print(f"  control {target:<26} {state}   {summary_line(output)}")
        ok = ok and code == 0

    for obj in object_files(BUILD_PY, "_flox_py"):
        obj.unlink()
    try:
        cmake_build(BUILD_PY, "_flox_py")
    except BuildFailed as e:
        print(f"  control _flox_py: BUILD FAILED\n{e.output[-3000:]}")
        ok = False
    code, output = run_pytest(PY_TEST_FILES)
    print(f"  control {'python/tests':<26} {'green' if code == 0 else 'RED'}   "
          f"{summary_line(output)}")
    ok = ok and code == 0

    try:
        cmake_build(BUILD, JS_RUNNER_TARGET)
    except BuildFailed as e:
        print(f"  control {JS_RUNNER_TARGET}: BUILD FAILED\n{e.output[-3000:]}")
        ok = False
    ex_ok, ex_detail = run_examples()
    print(f"  control {'examples (14)':<26} {'green' if ex_ok else 'RED'}")
    if not ex_ok:
        print(ex_detail)
    ok = ok and ex_ok

    return ok


def primary_check(m: Mutation) -> tuple[bool, str]:
    """(red, detail) for the mutation's own declared check. red=True means killed."""
    if m.kind == "cpp":
        removed = object_files(BUILD, m.cpp_target)
        for f in m.files():
            removed += object_files_for_source(BUILD, f)
        for obj in set(removed):
            obj.unlink()
        try:
            output = cmake_build(BUILD, m.cpp_target)
        except BuildFailed as e:
            print("  DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            raise
        compiled = require_compiled(output, m.cpp_target)
        print(f"  removed {len(removed)} object file(s), rebuilt {m.cpp_target} "
              f"({compiled} 'Building CXX' line(s))")
        code, test_output = run_gtest(m.cpp_target, m.gtest_filter)
        red = code != 0
        shown = m.gtest_filter or "(whole binary)"
        print(f"  {m.cpp_target} --gtest_filter={shown} -> exit {code} "
              f"({'RED, killed' if red else 'GREEN'})   {summary_line(test_output)}")
        if not red:
            print("\n".join(test_output.splitlines()[-15:]))
        return red, ""

    if m.kind == "python":
        removed = object_files(BUILD_PY, "_flox_py")
        for f in m.files():
            removed += object_files_for_source(BUILD_PY, f)
        for obj in set(removed):
            obj.unlink()
        try:
            output = cmake_build(BUILD_PY, "_flox_py")
        except BuildFailed as e:
            print("  DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            raise
        compiled = require_compiled(output, "_flox_py")
        print(f"  removed {len(removed)} object file(s), rebuilt _flox_py "
              f"({compiled} 'Building CXX' line(s))")
        code, test_output = run_pytest(m.pytest_ids)
        red = code != 0
        print(f"  pytest {m.pytest_ids} -> exit {code} ({'RED, killed' if red else 'GREEN'})   "
              f"{summary_line(test_output)}")
        if not red:
            print("\n".join(test_output.splitlines()[-15:]))
        return red, ""

    if m.kind == "text" and m.pytest_ids:
        # Pure prose/comment edit: nothing compiles it, the pytest reads the
        # file straight off disk.
        code, test_output = run_pytest(m.pytest_ids)
        red = code != 0
        print(f"  pytest {m.pytest_ids} -> exit {code} ({'RED, killed' if red else 'GREEN'})   "
              f"{summary_line(test_output)}")
        if not red:
            print("\n".join(test_output.splitlines()[-15:]))
        return red, ""

    # kind pyi / js: no primary check at all.
    print("  no primary check for this file -- straight to the sweep")
    return False, ""


def sweep(m: Mutation) -> tuple[bool, str]:
    """Re-run every other given surface. (caught, detail)."""
    print("  sweep: the other cpp binaries, both python test files, all examples")
    for target in CPP_TARGETS:
        if m.kind == "cpp" and target == m.cpp_target:
            continue  # already the primary check
        try:
            cmake_build(BUILD, target)
        except BuildFailed as e:
            return True, f"{target} failed to rebuild during the sweep:\n{e.output[-2000:]}"
        code, output = run_gtest(target, None)
        if code != 0:
            return True, f"{target} (whole binary) -> RED   {summary_line(output)}"

    if m.kind != "python":
        try:
            cmake_build(BUILD_PY, "_flox_py")
        except BuildFailed as e:
            return True, f"_flox_py failed to rebuild during the sweep:\n{e.output[-2000:]}"
    code, output = run_pytest(PY_TEST_FILES)
    if code != 0:
        return True, f"python/tests (both files) -> RED   {summary_line(output)}"

    if m.kind == "cpp":
        try:
            cmake_build(BUILD, JS_RUNNER_TARGET)
        except BuildFailed as e:
            return True, f"{JS_RUNNER_TARGET} failed to rebuild during the sweep:\n{e.output[-2000:]}"
    ex_ok, ex_detail = run_examples()
    if not ex_ok:
        return True, f"examples runner -> a script failed:\n{ex_detail}"

    return False, ""


def run_mutation(m: Mutation, skip_sweep: bool) -> tuple[str, str]:
    """(verdict, detail). verdict in killed/killed-elsewhere/alive/equivalent/no-compile."""
    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    print(f"\n[{m.name}]  kind={m.kind}")
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
    detail = ""
    try:
        try:
            red, _ = primary_check(m)
        except BuildFailed:
            return "no-compile", "did not compile"

        if red:
            verdict = "killed"
        elif skip_sweep:
            verdict = "alive"
            detail = "sweep skipped (--skip-sweep)"
        else:
            caught, sweep_detail = sweep(m)
            if caught:
                verdict = "killed-elsewhere"
                detail = sweep_detail
                print(f"  CAUGHT ELSEWHERE: {sweep_detail}")
            elif m.equivalent_reason:
                verdict = "equivalent"
                detail = m.equivalent_reason
                print(f"  EQUIVALENT: {detail}")
            else:
                verdict = "alive"
                print("  GREEN EVERYWHERE -- a hole")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for target in CPP_TARGETS:
            for obj in object_files(BUILD, target):
                obj.unlink()
        for obj in object_files(BUILD_PY, "_flox_py"):
            obj.unlink()

    return verdict, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--skip-control", action="store_true",
                        help="skip the before/after full control run (for --only debugging)")
    parser.add_argument("--skip-sweep", action="store_true",
                        help="skip the cross-surface sweep for survivors (primary check only)")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            target = m.cpp_target or (", ".join(m.pytest_ids) if m.pytest_ids else "(no primary check)")
            print(f"{m.name:<48} {m.kind:<7} {target}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD} is not configured; see the module docstring")
    if not (BUILD_PY / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD_PY} is not configured; see the module docstring")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    if not args.skip_control:
        print("control run before the mutations")
        if not control():
            raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m, args.skip_sweep)) for m in selected]

    restored = True
    if not args.skip_control:
        print("\ncontrol run after the mutations")
        restored = control()

    print("\nsummary")
    label = {"killed": "RED       ", "killed-elsewhere": "RED(else) ", "alive": "ALIVE     ",
             "equivalent": "EQUIVALENT", "no-compile": "NOBUILD   "}
    for m, verdict, detail in results:
        extra = f"  ({detail})" if detail and verdict != "alive" else ""
        print(f"  {label[verdict]}  {m.name:<48} {m.kind:<7} {', '.join(m.files())}{extra}")

    survived = [(m.name, m.why) for m, v, d in results if v == "alive"]
    equivalent = [(m.name, d) for m, v, d in results if v == "equivalent"]
    killed_elsewhere = [(m.name, d) for m, v, d in results if v == "killed-elsewhere"]
    nobuild = [m.name for m, v, d in results if v == "no-compile"]

    if survived:
        print(f"\n{len(survived)} mutation(s) survived everywhere -- holes:")
        for name, why in survived:
            print(f"  - {name}: {why}")
    if equivalent:
        print(f"\n{len(equivalent)} mutation(s) survived but are equivalent (documented, not "
              f"counted as failures):")
        for name, reason in equivalent:
            print(f"  - {name}: {reason}")
    if killed_elsewhere:
        print(f"\n{len(killed_elsewhere)} mutation(s) were caught by the sweep, not the primary "
              f"check:")
        for name, detail in killed_elsewhere:
            print(f"  - {name}: {detail.splitlines()[0] if detail else ''}")
    if nobuild:
        print(f"\n{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")

    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
