#!/usr/bin/env python3
"""Mutation harness for the Python binding exception/GIL contract.

python_gates_bindings_key/summary: RiskManager/KillSwitch/OrderValidator must
deny when their Python callable raises, the nine strategy bridge callbacks
must not let a Python exception cross the C ABI, and Runner.on_trade /
on_book_snapshot / on_bar must not publish while holding the GIL. This script
breaks that contract one piece at a time, in python/hook_bindings.h,
python/strategy_bindings.h and python/callback_guard.h, and checks that the
tests written for it go red.

Two test surfaces answer for every mutation:

    the relevant files   python/tests/test_binding_callback_exceptions.py
                         python/tests/test_hooks.py
                         python/tests/test_log_callback_teardown.py
    the whole directory  python/tests

A mutation is only reported as a genuine hole if it stays green against BOTH:
the relevant files decide kill/survive per mutation (fast), and every mutation
that survives them is re-run against the whole python/tests directory before
it is trusted, in case something elsewhere in the suite happens to catch it.

Every run is honest about the build: the mutated file's hash is printed
before and after, the target's own object files are deleted so nothing is
served from cache, the rebuild output has to contain "Building CXX" or the
run is refused, and every pytest invocation runs under a timeout. A mutation
that does not compile is not a mutation.

Usage:

    python3 scripts/mutations/python_gates.py             # control, mutations, control
    python3 scripts/mutations/python_gates.py --list
    python3 scripts/mutations/python_gates.py --only gate-risk-manager-starts-allowed
    python3 scripts/mutations/python_gates.py --skip-full-suite-check

The build directory is expected to be configured already, the way CI does it:

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLOX_BUILD_PYTHON=ON \\
          -DFLOX_ENABLE_BACKTEST=ON -Dpybind11_DIR=<pybind11.get_cmake_dir()>
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
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 300  # the relevant files alone; individual drive() cases carry their own

_venv_python = REPO / ".venv" / "bin" / "python"
PYTHON = str(_venv_python) if _venv_python.is_file() else sys.executable
PYTHONPATH = str(BUILD / "python")

TARGET = "_flox_py"

HOOKS = "python/hook_bindings.h"
STRATEGY = "python/strategy_bindings.h"
GUARD = "python/callback_guard.h"

RELEVANT_TESTS = [
    "python/tests/test_binding_callback_exceptions.py",
    "python/tests/test_hooks.py",
    "python/tests/test_log_callback_teardown.py",
]
FULL_SUITE = "python/tests"


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
    occurrence: int = 1
    expected_occurrences: int = 1
    # An "equivalent" mutation is expected to survive for a documented reason
    # that is not a hole in the tests (e.g. the mutated return value has no
    # caller that reads it). It still runs; it just does not fail the script.
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
    # ── the three pre-trade gate bridges, individually, starting from allow ──
    Mutation(
        name="gate-risk-manager-bridge-starts-allowed",
        why="riskAllowBridge's default answer before the callable runs is flipped to allow, "
            "so a callable that raises (never reaching the assignment) lets the signal through",
        file=HOOKS,
        old="uint8_t result = kGateDenied;",
        new="uint8_t result = kGateAllowed;",
        occurrence=1,
        expected_occurrences=3,
    ),
    Mutation(
        name="gate-kill-switch-bridge-starts-allowed",
        why="killCheckBridge's default is flipped to allow: a KillSwitch.check that raises "
            "lets trading continue instead of halting it",
        file=HOOKS,
        old="uint8_t result = kGateDenied;",
        new="uint8_t result = kGateAllowed;",
        occurrence=2,
        expected_occurrences=3,
    ),
    Mutation(
        name="gate-order-validator-bridge-starts-allowed",
        why="orderValidateBridge's default is flipped to allow: a validator that raises "
            "accepts the order it never actually validated",
        file=HOOKS,
        old="uint8_t result = kGateDenied;",
        new="uint8_t result = kGateAllowed;",
        occurrence=3,
        expected_occurrences=3,
    ),
    # ── the three BacktestRunner-side C++ adapters, individually, starting from allow ──
    Mutation(
        name="backtest-risk-manager-adapter-starts-allowed",
        why="PyRiskManagerCxxAdapter::allow defaults to true before the callable runs, so a "
            "raise on the BacktestRunner attach point fills instead of blocking",
        file=HOOKS,
        old="bool result = false;",
        new="bool result = true;",
    ),
    Mutation(
        name="backtest-kill-switch-adapter-starts-allowed",
        why="PyKillSwitchCxxAdapter::check defaults `allowed` to true, so a raising check "
            "never latches _triggered and the backtest keeps trading",
        file=HOOKS,
        old="bool allowed = false;",
        new="bool allowed = true;",
    ),
    Mutation(
        name="backtest-order-validator-adapter-starts-allowed",
        why="PyOrderValidatorCxxAdapter::validate defaults `valid` to true, so a raising "
            "validator accepts every order on the BacktestRunner path",
        file=HOOKS,
        old="bool valid = false;",
        new="bool valid = true;",
    ),
    # ── a non-bool return from the callable is treated as allow ──
    Mutation(
        name="risk-manager-non-bool-return-is-allow",
        why="the trampoline stops using pybind11's strict bool cast and instead denies only "
            "on a literal Python `False`; a non-bool answer (None, 0, \"\", an object) is "
            "treated as allow instead of whatever the real coercion/error path would do",
        file=HOOKS,
        old="""  bool allow(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(bool, PyRiskManager, allow, sig);
  }""",
        new="""  bool allow(const PySignal& sig) override
  {
    py::gil_scoped_acquire gil;
    py::function override_fn =
        pybind11::get_override(static_cast<const PyRiskManager*>(this), "allow");
    if (override_fn)
    {
      py::object o = override_fn(sig);
      // BUG: only a literal Python `False` denies; every other answer --
      // including falsy non-bool values like 0, "", None -- is allow.
      return !(py::isinstance<py::bool_>(o) && !o.cast<bool>());
    }
    return PyRiskManager::allow(sig);
  }""",
    ),
    # ── invokeUnderGil returns true after an exception ──
    Mutation(
        name="invoke-under-gil-returns-true-after-exception",
        why="invokeUnderGil no longer propagates guardCallback's completion flag -- it always "
            "reports success even when the callable raised",
        file=HOOKS,
        old="  return guardCallback(fn, source, loc);",
        new="  guardCallback(fn, source, loc);\n  return true;",
        equivalent_reason="every call site (all 52 in python/*.h) invokes invokeUnderGil / "
            "guardCallback as a bare statement and never reads the returned bool -- the "
            "correctness of every gate and callback rests on the `result`/`fired` variable "
            "captured *inside* the guarded lambda, not on this return value. `grep -n "
            "'= invokeUnderGil\\|= guardCallback' python/*.h` is empty. This mutation is dead "
            "code with today's call sites, so it is not a hole in the tests -- it is checked "
            "for completeness because the task named it explicitly.",
    ),
    # ── exception text not reported ──
    Mutation(
        name="guard-callback-reports-empty-message",
        why="the py::error_already_set catch in guardCallback reports an empty string instead "
            "of e.what(), so the description never actually describes anything",
        file=GUARD,
        old="detail::reportCallbackError(label, e.what());",
        new='detail::reportCallbackError(label, "");',
        occurrence=1,
        expected_occurrences=2,
    ),
    # ── reported at info level instead of error ──
    Mutation(
        name="report-callback-error-logs-at-info",
        why="the engine log call in reportCallbackError drops from Error to Info; the policy "
            "in callback_guard.h promises Error",
        file=GUARD,
        old="flox::LogStream(flox::LogLevel::Error) << text;",
        new="flox::LogStream(flox::LogLevel::Info) << text;",
        equivalent_reason="",
    ),
    # ── nine strategy bridges bypassing the dispatch helper, individually ──
    Mutation(
        name="strategy-bridge-on-trade-bypasses-dispatch",
        why="onTrade calls the lambda directly instead of through PyStrategyHost::dispatch, "
            "so a raise in Strategy.on_trade is unguarded again",
        file=STRATEGY,
        old='dispatch(self, "Strategy.on_trade", call);',
        new="call();",
    ),
    Mutation(
        name="strategy-bridge-on-book-update-bypasses-dispatch",
        why="onBook calls the lambda directly instead of through dispatch",
        file=STRATEGY,
        old='dispatch(self, "Strategy.on_book_update", call);',
        new="call();",
    ),
    Mutation(
        name="strategy-bridge-on-bar-bypasses-dispatch",
        why="onBar calls the lambda directly instead of through dispatch",
        file=STRATEGY,
        old='dispatch(self, "Strategy.on_bar", call);',
        new="call();",
    ),
    Mutation(
        name="strategy-bridge-on-start-bypasses-dispatch",
        why="onStart calls Strategy.on_start directly, skipping dispatch entirely",
        file=STRATEGY,
        old="""  static void onStart(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    dispatch(self, "Strategy.on_start",
             [self]
             { self->strategy.load(std::memory_order_acquire)->on_start(); });
  }""",
        new="""  static void onStart(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    self->strategy.load(std::memory_order_acquire)->on_start();
  }""",
    ),
    Mutation(
        name="strategy-bridge-on-stop-bypasses-dispatch",
        why="onStop calls Strategy.on_stop directly, skipping dispatch entirely",
        file=STRATEGY,
        old="""  static void onStop(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    dispatch(self, "Strategy.on_stop",
             [self]
             { self->strategy.load(std::memory_order_acquire)->on_stop(); });
  }""",
        new="""  static void onStop(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    self->strategy.load(std::memory_order_acquire)->on_stop();
  }""",
    ),
    Mutation(
        name="strategy-bridge-on-fill-bypasses-dispatch",
        why="onFill calls the lambda directly instead of through dispatch -- this is the "
            "BacktestRunner attach point, where the original bug tore the run down mid-run",
        file=STRATEGY,
        old='dispatch(self, "Strategy.on_fill", call);',
        new="call();",
    ),
    Mutation(
        name="strategy-bridge-on-order-update-bypasses-dispatch",
        why="onOrderUpdate calls the lambda directly instead of through dispatch",
        file=STRATEGY,
        old='dispatch(self, "Strategy.on_order_update", call);',
        new="call();",
    ),
    Mutation(
        name="strategy-bridge-on-queue-position-change-bypasses-dispatch",
        why="onQueuePositionChange calls the lambda directly instead of through dispatch",
        file=STRATEGY,
        old='dispatch(self, "Strategy.on_queue_position_change", call);',
        new="call();",
    ),
    Mutation(
        name="strategy-bridge-on-market-position-change-bypasses-dispatch",
        why="onMarketPositionChange calls the lambda directly instead of through dispatch -- "
            "this is the venue-stack attach point",
        file=STRATEGY,
        old='dispatch(self, "Strategy.on_market_position_change", call);',
        new="call();",
    ),
    # ── the with_gil branch acquires the GIL on the wrong side ──
    Mutation(
        name="strategy-host-dispatch-with-gil-inverted",
        why="dispatch's with_gil test is inverted: the live/threaded path (with_gil=true, a "
            "bus consumer thread with no GIL of its own) now calls into Python without "
            "acquiring it, and the sync path acquires a GIL it already holds",
        file=STRATEGY,
        old="""  template <typename Fn>
  static void dispatch(const PyStrategyHost* self, const char* source, Fn&& fn)
  {
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      flox_py::guardCallback(fn, source);
    }
    else
    {
      flox_py::guardCallback(fn, source);
    }
  }""",
        new="""  template <typename Fn>
  static void dispatch(const PyStrategyHost* self, const char* source, Fn&& fn)
  {
    if (!self->with_gil)
    {
      py::gil_scoped_acquire gil;
      flox_py::guardCallback(fn, source);
    }
    else
    {
      flox_py::guardCallback(fn, source);
    }
  }""",
    ),
    # ── gil_scoped_release removed from the three publish_* methods ──
    Mutation(
        name="publish-trade-holds-the-gil",
        why="PyLiveEngine::publish_trade no longer releases the GIL before publishing, so a "
            "consumer blocked in its Python callback past the ring cannot get the GIL back "
            "to drain it",
        file=STRATEGY,
        old="""    py::gil_scoped_release release;
    flox_live_engine_publish_trade(_engine, symbol, price, qty,""",
        new="    flox_live_engine_publish_trade(_engine, symbol, price, qty,",
    ),
    Mutation(
        name="publish-book-snapshot-holds-the-gil",
        why="PyLiveEngine::publish_book_snapshot no longer releases the GIL before publishing",
        file=STRATEGY,
        old="""    py::gil_scoped_release release;
    flox_live_engine_publish_book_snapshot(_engine, symbol,""",
        new="    flox_live_engine_publish_book_snapshot(_engine, symbol,",
    ),
    Mutation(
        name="publish-bar-holds-the-gil",
        why="PyLiveEngine::publish_bar no longer releases the GIL before publishing",
        file=STRATEGY,
        old="""    py::gil_scoped_release release;
    flox_live_engine_publish_bar(_engine, symbol, bar_type, bar_type_param,""",
        new="    flox_live_engine_publish_bar(_engine, symbol, bar_type, bar_type_param,",
    ),
    # ── the side SIGSEGV fix, undone piece by piece ──
    Mutation(
        name="log-callback-atexit-hook-not-registered",
        why="the atexit.register(logCallbackTeardown) call at module init is removed, so a "
            "Python log callback left installed at interpreter shutdown is released -- and "
            "can still be called by a C++ thread -- after finalisation",
        file=STRATEGY,
        old="""  py::module_::import("atexit").attr("register")(
      py::cpp_function(&flox_py::logCallbackTeardown));
""",
        new="",
    ),
    Mutation(
        name="log-callback-slot-destroyed-at-static-destruction-again",
        why="globalLogCallback() goes back to a plain function-static py::object instead of "
            "the deliberately-leaked heap pointer; its destructor now runs after the "
            "interpreter is gone, which is the original SIGSEGV",
        file=HOOKS,
        old="""inline py::object& globalLogCallback()
{
  static py::object* cb = new py::object();
  return *cb;
}""",
        new="""inline py::object& globalLogCallback()
{
  static py::object cb;
  return cb;
}""",
    ),
    Mutation(
        name="recursion-guard-on-raising-log-sink-removed",
        why="reportCallbackError no longer short-circuits when it is already reporting on this "
            "thread, so a Python log sink that itself raises recurses back into "
            "reportCallbackError through loggerBridge -> invokeUnderGil, without bound",
        file=GUARD,
        old="""  if (g_reportingCallbackError)
  {
    // The log sink raised while carrying an earlier report. Stderr is
    // the only place left, for this message and for the one underneath.
    g_logSinkRaised = true;
    PySys_WriteStderr("%s\\n", text.c_str());
    return;
  }

  ReportingScope scope;""",
        new="  ReportingScope scope;",
    ),
    Mutation(
        name="fallback-to-stderr-removed",
        why="the final `if (!delivered)` fallback in reportCallbackError is removed, so a "
            "description that could not be delivered through the engine log (logging "
            "disabled, or the sink itself raised) is lost instead of reaching stderr",
        file=GUARD,
        old="""  if (!delivered)
  {
    // Losing the description is not an option.
    PySys_WriteStderr("%s\\n", text.c_str());
  }
}""",
        new="}",
    ),
    # ── adversarial mutations of our own ──────────────────────────────────
    Mutation(
        name="adv-kill-switch-bridge-ignores-the-answer",
        why="killCheckBridge always returns kGateDenied, whatever KillSwitch.check() answers. "
            "Every test that exercises KillSwitch (test_kill_switch_drops_signal, the raise "
            "cases) expects a deny and still gets one -- nothing in python/tests ever gives a "
            "KillSwitch a chance to allow and checks that the signal got through, so the "
            "allow half of the contract is unguarded on this bridge",
        file=HOOKS,
        old="""inline uint8_t killCheckBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyKillSwitch*>(ud);
  // Deny unless the callable says otherwise: a gate that raised, or
  // answered with something that is not a bool, did not let the signal
  // through. See callback_guard.h.
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->check(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "KillSwitch.check");
  return result;
}""",
        new="""inline uint8_t killCheckBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyKillSwitch*>(ud);
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->check(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "KillSwitch.check");
  (void)result;
  // BUG: the callable's answer is computed and then thrown away.
  return kGateDenied;
}""",
    ),
    Mutation(
        name="adv-order-validator-bridge-ignores-the-answer",
        why="orderValidateBridge always returns kGateDenied, the same gap as the KillSwitch "
            "one above: test_order_validator_drops_signal expects a deny and still gets one, "
            "and nothing in python/tests ever checks that OrderValidator.validate() returning "
            "True lets an order through this bridge",
        file=HOOKS,
        old="""inline uint8_t orderValidateBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyOrderValidator*>(ud);
  // Deny unless the callable says otherwise: a gate that raised, or
  // answered with something that is not a bool, did not let the signal
  // through. See callback_guard.h.
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->validate(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "OrderValidator.validate");
  return result;
}""",
        new="""inline uint8_t orderValidateBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyOrderValidator*>(ud);
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->validate(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "OrderValidator.validate");
  (void)result;
  // BUG: the callable's answer is computed and then thrown away.
  return kGateDenied;
}""",
    ),
    Mutation(
        name="adv-report-once-per-source-then-never-again",
        why="reportCallbackError remembers every source it has already reported and silently "
            "drops every later report from that same source. Every acceptance case raises "
            "exactly once per driver process (each STRATEGY_CALLBACKS / GATES case triggers "
            "its raise on the first invocation only, per `if fired[0] == 1: raise`), so the "
            "one report each test looks for still gets through and the dedup never shows",
        edits=[
            Edit(
                file=GUARD,
                old="#include <cstdint>\n#include <exception>\n#include <source_location>\n#include <string>",
                new="#include <cstdint>\n#include <exception>\n#include <set>\n"
                    "#include <source_location>\n#include <string>",
            ),
            Edit(
                file=GUARD,
                old="""  const std::string text =
      std::string("flox: ") + (source ? source : "a Python callback") + " raised: " + what;

  if (g_reportingCallbackError)""",
                new="""  const std::string text =
      std::string("flox: ") + (source ? source : "a Python callback") + " raised: " + what;

  // BUG: a source that has already reported once never reports again,
  // even on a later, distinct raise.
  static std::set<std::string> reportedSources;
  if (!reportedSources.insert(source ? source : "").second)
  {
    return;
  }

  if (g_reportingCallbackError)""",
            ),
        ],
    ),
    Mutation(
        name="adv-risk-manager-latches-deny-after-first-raise",
        why="riskAllowBridge latches: once RiskManager.allow() has raised once, every later "
            "call denies without even invoking the callable again -- whatever it would have "
            "answered. No test in python/tests calls a gate a second time after it raised and "
            "checks that a later, non-raising call still gets a real answer",
        file=HOOKS,
        old="""inline uint8_t riskAllowBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyRiskManager*>(ud);
  // Deny unless the callable says otherwise: a gate that raised, or
  // answered with something that is not a bool, did not let the signal
  // through. See callback_guard.h.
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->allow(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "RiskManager.allow");
  return result;
}""",
        new="""inline uint8_t riskAllowBridge(void* ud, const FloxSignal* sig)
{
  static bool everRaised = false;
  if (everRaised)
  {
    return kGateDenied;
  }
  auto* py_obj = static_cast<PyRiskManager*>(ud);
  uint8_t result = kGateDenied;
  bool completed = invokeUnderGil(
      [&]
      { result = py_obj->allow(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
      "RiskManager.allow");
  if (!completed)
  {
    everRaised = true;
  }
  return result;
}""",
    ),
    Mutation(
        name="adv-threaded-dispatch-drops-the-guard-only",
        why="dispatch keeps acquiring the GIL correctly on the with_gil (live bus consumer) "
            "path, but calls fn() directly instead of through guardCallback there -- only the "
            "containment is gone, not the GIL discipline. The sync path is untouched",
        file=STRATEGY,
        old="""  template <typename Fn>
  static void dispatch(const PyStrategyHost* self, const char* source, Fn&& fn)
  {
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      flox_py::guardCallback(fn, source);
    }
    else
    {
      flox_py::guardCallback(fn, source);
    }
  }""",
        new="""  template <typename Fn>
  static void dispatch(const PyStrategyHost* self, const char* source, Fn&& fn)
  {
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      fn();
    }
    else
    {
      flox_py::guardCallback(fn, source);
    }
  }""",
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
    out = subprocess.run(
        ["find", str(BUILD), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, output: str):
        super().__init__("rebuild failed")
        self.output = output


def rebuild() -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", TARGET, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(output)
    if "Building CXX" not in output:
        raise SystemExit(
            "rebuild compiled nothing -- the result would have been a stale .so, so the run "
            f"is refused:\n{output[-2000:]}"
        )
    return output


def run_pytest(paths: list[str], timeout: int) -> tuple[int, str]:
    env = dict(os.environ)
    env["PYTHONPATH"] = PYTHONPATH
    cmd = [PYTHON, "-m", "pytest", *paths, "-q"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO,
                                env=env)
    except subprocess.TimeoutExpired as expired:
        out = expired.stdout or ""
        err = expired.stderr or ""
        if isinstance(out, bytes):
            out = out.decode(errors="replace")
        if isinstance(err, bytes):
            err = err.decode(errors="replace")
        return 124, f"timed out after {timeout}s\n{out}{err}"
    return result.returncode, result.stdout + result.stderr


def summary_line(output: str) -> str:
    """pytest's last non-blank output line, e.g. '3 passed, 1 failed in 0.4s'."""
    tail = [line for line in output.splitlines() if line.strip()]
    return tail[-1].strip() if tail else ""


def control(paths: list[str], timeout: int, label: str) -> bool:
    for obj in object_files(TARGET):
        obj.unlink()
    output = rebuild()
    compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
    code, test_output = run_pytest(paths, timeout)
    state = "green" if code == 0 else "RED"
    print(f"  control [{label}] rebuilt ({compiled} 'Building CXX' line(s)) -> exit {code} "
          f"({state})   {summary_line(test_output)}")
    if code != 0:
        print(test_output[-4000:])
    return code == 0


def run_mutation(m: Mutation) -> tuple[str, str]:
    """(verdict, detail). verdict in killed/alive/equivalent/no-compile."""
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
    detail = ""
    try:
        removed = object_files(TARGET)
        for obj in removed:
            obj.unlink()
        print(f"  removed {len(removed)} object file(s) for {TARGET}")

        try:
            output = rebuild()
            compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
            print(f"  rebuilt {TARGET}: {compiled} 'Building CXX' line(s)")
        except BuildFailed as e:
            print("  DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-20:]))
            return "no-compile", "did not compile"

        code, test_output = run_pytest(RELEVANT_TESTS, TEST_TIMEOUT)
        red = code != 0
        print(f"  relevant tests -> exit {code} ({'RED, mutation killed' if red else 'green'})"
              f"   {summary_line(test_output)}")
        if red:
            verdict = "killed"
            failed = [line for line in test_output.splitlines()
                      if line.startswith("FAILED ") or line.startswith("ERROR ")]
            for line in failed[:10]:
                print(f"    {line}")
        else:
            # Green against the relevant files alone is not yet a verdict:
            # confirm it against the whole directory before calling it a hole.
            full_code, full_output = run_pytest([FULL_SUITE], TEST_TIMEOUT)
            if full_code != 0:
                verdict = "killed-elsewhere"
                detail = "relevant files green, caught by the rest of python/tests"
                print(f"  full suite -> exit {full_code} (RED, {detail})")
                failed = [line for line in full_output.splitlines()
                          if line.startswith("FAILED ") or line.startswith("ERROR ")]
                for line in failed[:10]:
                    print(f"    {line}")
            else:
                verdict = "alive"
                print("  full suite -> green too")
                if m.equivalent_reason:
                    verdict = "equivalent"
                    detail = m.equivalent_reason
                    print(f"  EQUIVALENT: {detail}")
                else:
                    print("  GREEN, MUTATION SURVIVED -- a hole in the tests")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for obj in object_files(TARGET):
            obj.unlink()

    return verdict, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--skip-control", action="store_true",
                        help="skip the before/after control runs (for --only debugging)")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<52} {', '.join(m.files())}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD} is not configured; run cmake -B build ... first")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    if not args.skip_control:
        print("control run before the mutations (relevant files)")
        if not control(RELEVANT_TESTS, TEST_TIMEOUT, "relevant"):
            raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m)) for m in selected]

    restored = True
    if not args.skip_control:
        print("\ncontrol run after the mutations (relevant files)")
        restored = control(RELEVANT_TESTS, TEST_TIMEOUT, "relevant")

    print("\nsummary")
    label = {"killed": "RED       ", "killed-elsewhere": "RED(else) ", "alive": "ALIVE     ",
             "equivalent": "EQUIVALENT", "no-compile": "NOBUILD   "}
    for m, verdict, detail in results:
        extra = f"  ({detail})" if detail else ""
        print(f"  {label[verdict]}  {m.name:<52} {', '.join(m.files())}{extra}")

    survived = [(m.name, d) for m, v, d in results if v == "alive"]
    equivalent = [(m.name, d) for m, v, d in results if v == "equivalent"]
    killed_elsewhere = [(m.name, d) for m, v, d in results if v == "killed-elsewhere"]
    nobuild = [m.name for m, v, d in results if v == "no-compile"]

    if survived:
        print(f"\n{len(survived)} mutation(s) survived -- holes in the tests:")
        for name, _ in survived:
            print(f"  - {name}")
    if equivalent:
        print(f"\n{len(equivalent)} mutation(s) survived but are equivalent (documented, not "
              f"counted as failures):")
        for name, reason in equivalent:
            print(f"  - {name}: {reason}")
    if killed_elsewhere:
        print(f"\n{len(killed_elsewhere)} mutation(s) were green on the relevant files but "
              f"caught by the rest of python/tests:")
        for name, reason in killed_elsewhere:
            print(f"  - {name}")
    if nobuild:
        print(f"\n{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")

    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
