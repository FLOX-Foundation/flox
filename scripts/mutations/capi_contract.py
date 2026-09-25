#!/usr/bin/env python3
"""Mutation harness for the C API contract fix: the canonical order-type
table (python/order_type_bindings.h + include/flox/capi/order_type_names.hpp),
the C ABI version handshake run at load by Python/Node/QuickJS
(include/flox/capi/abi_check.hpp + the four call sites), the VenueExecutor
trigger fix (python/bare_executor_bindings.h), the simulated executor no
longer rewriting a conditional order's type (src/backtest/simulated_executor.cpp),
and the ownership notes in include/flox/capi/flox_capi.h.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each piece of the fix one at
a time and checks that the test written for it goes red -- and, for every
mutation that stays green, sweeps the whole named test surface (python/tests,
every build/tests/test_capi_* binary, test_quickjs, the Node suite) before
calling it a survivor.

Three build trees are involved, because the fix spans three bindings plus a
handful of shared headers compiled separately into each of them:

    build/      -DFLOX_BUILD_TESTS=ON -DFLOX_BUILD_CAPI=ON -DFLOX_BUILD_QUICKJS=ON
                -DFLOX_ENABLE_BACKTEST=ON      (gtest binaries, one per test file)
    build-py/   -DFLOX_BUILD_PYTHON=ON -DFLOX_ENABLE_BACKTEST=ON, against .venv-tests
                (the _flox_py extension -- its own copy of libflox, not build/'s)
    node/build/ cmake-js against build/'s libflox (FLOX_BUILD_CAPI=ON there)

Every rebuild is honest about it: the mutated file's hash is printed before
and after, the affected target's own object files are deleted first so
nothing is served from cache, a C++ rebuild has to contain "Building CXX" or
the run is refused, a Node rebuild has to relink flox_node.node (size/mtime
checked), and every test run carries a timeout. A mutation that does not
compile is not a mutation.

Usage:

    python3 scripts/mutations/capi_contract.py                # control, all mutations, control
    python3 scripts/mutations/capi_contract.py --list
    python3 scripts/mutations/capi_contract.py --only ordertype-iceberg-missing-from-table
    python3 scripts/mutations/capi_contract.py --skip-sweep    # primary check only (fast iteration)

Prerequisites (not done by this script -- see the worktree's own setup):

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLOX_BUILD_TESTS=ON \\
          -DFLOX_BUILD_CAPI=ON -DFLOX_BUILD_QUICKJS=ON -DFLOX_ENABLE_BACKTEST=ON
    cmake --build build -j8

    python3 -m venv .venv-tests && .venv-tests/bin/python -m pip install \\
          "pybind11<3.1.0" numpy pytest
    cmake -S . -B build-py -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLOX_BUILD_PYTHON=ON \\
          -DFLOX_ENABLE_BACKTEST=ON -Dpybind11_DIR=$(.venv-tests/bin/python -c \\
          "import pybind11; print(pybind11.get_cmake_dir())")
    cmake --build build-py --target _flox_py -j8

    cd node && npm install && npm run build
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
NODE_DIR = REPO / "node"
NODE_ADDON = NODE_DIR / "build" / "Release" / "flox_node.node"

CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
NPM = os.environ.get("NPM", "npm")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "8")
BUILD_TIMEOUT = 600
TEST_TIMEOUT = 120
SWEEP_PY_TIMEOUT = 180
SWEEP_NODE_TIMEOUT = 60

_venv_python = REPO / ".venv-tests" / "bin" / "python"
PYTHON = str(_venv_python) if _venv_python.is_file() else sys.executable
PYTHONPATH = str(BUILD_PY / "python")

PY_TARGET = "_flox_py"

# ── files touched, by name used in the Mutation table below ────────────────
ORDER_TYPE_NAMES_HPP = "include/flox/capi/order_type_names.hpp"
ABI_CHECK_HPP = "include/flox/capi/abi_check.hpp"
FLOX_CAPI_H = "include/flox/capi/flox_capi.h"
ORDER_TYPE_BINDINGS_H = "python/order_type_bindings.h"
BACKTEST_BINDINGS_H = "python/backtest_bindings.h"
BARE_EXECUTOR_BINDINGS_H = "python/bare_executor_bindings.h"
FLOX_PY_CPP = "python/flox_py.cpp"
BUNDLE_PY = "python/flox_py/bundle.py"
SIMULATED_EXECUTOR_CPP = "src/backtest/simulated_executor.cpp"
NODE_ABI_H = "node/src/abi.h"
JS_BINDINGS_CPP = "src/quickjs/js_bindings.cpp"
TRACE_HANDLERS_H = "include/flox/run/trace_handlers.h"


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
    kind: str  # "py" | "gtest" | "node"
    edits: list[Edit]
    # -- primary check --
    py_tests: list[str] = field(default_factory=list)       # kind == "py"
    gtest_target: str = ""                                  # kind == "gtest"
    gtest_filter: str | None = None                          # kind == "gtest"; None = whole binary
    node_test: str = ""                                      # kind == "node"
    # -- extra objects the primary rebuild must also delete (paths relative
    # to the tree's build dir) so a change to a shared header forces a real
    # recompile of the library translation unit that carries it --
    extra_py_objects: list[str] = field(default_factory=list)
    extra_gtest_objects: list[str] = field(default_factory=list)
    # -- sweep: which other trees / gtest targets to rebuild and re-run once
    # the primary check goes green, per the task's instruction to sweep
    # "the python tests, test_capi_*, test_quickjs, the Node suite" --
    sweep_rebuild_py: bool = False
    sweep_rebuild_gtest_targets: list[str] = field(default_factory=list)
    sweep_rebuild_node: bool = False
    equivalent_reason: str = ""

    def files(self) -> list[str]:
        seen: list[str] = []
        for e in self.edits:
            if e.file not in seen:
                seen.append(e.file)
        return seen


# ═════════════════════════════════════════════════════════════════════════
# 1. Order-type table -- the three former table sites, then the table itself
# ═════════════════════════════════════════════════════════════════════════

MUTATIONS: list[Mutation] = [
    Mutation(
        name="ordertype-unknown-to-market-backtest-bindings",
        why="PySimulatedExecutor::parseOrderType (backtest_bindings.h) stops delegating "
            "straight to the canonical flox_py::parseOrderTypeStrict and instead swallows its "
            "ValueError, falling back to OrderType::MARKET -- the first of the three former "
            "hand-written tables, reproduced at its call site instead of in a private copy",
        kind="py",
        edits=[Edit(
            file=BACKTEST_BINDINGS_H,
            old="""  static OrderType parseOrderType(const std::string& s)
  {
    return flox_py::parseOrderTypeStrict(s);
  }""",
            new="""  static OrderType parseOrderType(const std::string& s)
  {
    try
    {
      return flox_py::parseOrderTypeStrict(s);
    }
    catch (const std::invalid_argument&)
    {
      return OrderType::MARKET;
    }
  }""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
    ),
    Mutation(
        name="ordertype-unknown-to-market-bare-executor",
        why="VenueExecutor's parseOrderType lambda (bare_executor_bindings.h) swallows the "
            "strict parser's ValueError and falls back to MARKET -- the second former table",
        kind="py",
        edits=[Edit(
            file=BARE_EXECUTOR_BINDINGS_H,
            old="""  auto parseOrderType = [](const std::string& s) -> OrderType
  { return flox_py::parseOrderTypeStrict(s); };""",
            new="""  auto parseOrderType = [](const std::string& s) -> OrderType
  {
    try
    {
      return flox_py::parseOrderTypeStrict(s);
    }
    catch (const std::invalid_argument&)
    {
      return OrderType::MARKET;
    }
  };""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
    ),
    Mutation(
        name="ordertype-unknown-to-market-shared-strict",
        why="flox_py::parseOrderTypeStrict itself (order_type_bindings.h) -- the shared door "
            "both call sites above now go through -- falls back to MARKET on an unrecognised "
            "name instead of raising: the third former table, now centralised into one bug",
        kind="py",
        edits=[Edit(
            file=ORDER_TYPE_BINDINGS_H,
            old="""inline flox::OrderType parseOrderTypeStrict(const std::string& name)
{
  uint8_t code = 0;
  if (!flox::capi::orderTypeCodeFromName(name, &code))
  {
    throw std::invalid_argument(flox::capi::unknownOrderTypeMessage(name));
  }
  return static_cast<flox::OrderType>(code);
}""",
            new="""inline flox::OrderType parseOrderTypeStrict(const std::string& name)
{
  uint8_t code = 0;
  if (!flox::capi::orderTypeCodeFromName(name, &code))
  {
    return flox::OrderType::MARKET;
  }
  return static_cast<flox::OrderType>(code);
}""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
    ),

    # ── the canonical table itself (order_type_names.hpp): shared by all
    # four bindings, so these sweep all three build trees ──────────────────
    Mutation(
        name="ordertype-legacy-long-names-accepted",
        why="kOrderTypeNamesLower reverts codes 4/5 to the pre-fix "
            "\"take_profit_market\"/\"take_profit_limit\" instead of \"tp_market\"/\"tp_limit\" "
            "-- the exact drift the audit found between two of the three old tables",
        kind="py",
        edits=[Edit(
            file=ORDER_TYPE_NAMES_HPP,
            old="""inline constexpr const char* kOrderTypeNamesLower[] = {
    "limit",
    "market",
    "stop_market",
    "stop_limit",
    "tp_market",
    "tp_limit",
    "trailing_stop",
    "iceberg",
};""",
            new="""inline constexpr const char* kOrderTypeNamesLower[] = {
    "limit",
    "market",
    "stop_market",
    "stop_limit",
    "take_profit_market",
    "take_profit_limit",
    "trailing_stop",
    "iceberg",
};""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        extra_py_objects=[],
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_capi_signal_codes", "test_quickjs"],
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="ordertype-iceberg-missing-from-table",
        why="kOrderTypeNamesLower drops ICEBERG, shrinking kOrderTypeNameCount back to 7 -- "
            "the third former table's gap, now in the canonical table itself",
        kind="py",
        edits=[Edit(
            file=ORDER_TYPE_NAMES_HPP,
            old="""inline constexpr const char* kOrderTypeNamesLower[] = {
    "limit",
    "market",
    "stop_market",
    "stop_limit",
    "tp_market",
    "tp_limit",
    "trailing_stop",
    "iceberg",
};""",
            new="""inline constexpr const char* kOrderTypeNamesLower[] = {
    "limit",
    "market",
    "stop_market",
    "stop_limit",
    "tp_market",
    "tp_limit",
    "trailing_stop",
};""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_capi_signal_codes", "test_quickjs"],
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="ordertype-code-from-name-off-by-one",
        why="orderTypeCodeFromName writes i+1 instead of i on a match, so every name resolves "
            "to the next code's wire value -- round-tripping through name<->code silently shifts",
        kind="py",
        edits=[Edit(
            file=ORDER_TYPE_NAMES_HPP,
            old="""    if (name == kOrderTypeNamesLower[i])
    {
      *out = static_cast<uint8_t>(i);
      return true;
    }""",
            new="""    if (name == kOrderTypeNamesLower[i])
    {
      *out = static_cast<uint8_t>(i + 1);
      return true;
    }""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_capi_signal_codes", "test_quickjs"],
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="ordertype-valueerror-message-no-accepted-set",
        why="unknownOrderTypeMessage drops the accepted-set suffix, so the ValueError names "
            "what was rejected but never what would have been accepted",
        kind="py",
        edits=[Edit(
            file=ORDER_TYPE_NAMES_HPP,
            old="""inline std::string unknownOrderTypeMessage(const std::string& name)
{
  return "unknown order type '" + name + "'; expected one of: " + orderTypeNameList();
}""",
            new="""inline std::string unknownOrderTypeMessage(const std::string& name)
{
  return "unknown order type '" + name + "'";
}""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_capi_signal_codes", "test_quickjs"],
        sweep_rebuild_node=True,
    ),

    # ── order_type_bindings.h: the Python-only surface of the table ────────
    Mutation(
        name="ordertype-name-off-by-one",
        why="the order_type_name() Python function reads kOrderTypeNamesLower[(code+1) % "
            "count] instead of [code] -- flox.order_type_name(4) answers \"tp_limit\" instead "
            "of \"tp_market\"",
        kind="py",
        edits=[Edit(
            file=ORDER_TYPE_BINDINGS_H,
            old="        return std::string(flox::capi::kOrderTypeNamesLower[code]);",
            new="        return std::string(\n"
                "            flox::capi::kOrderTypeNamesLower[(code + 1) % flox::capi::kOrderTypeNameCount]);",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
    ),
    Mutation(
        name="ordertype-names-tuple-drops-entry",
        why="bindOrderTypes allocates and fills ORDER_TYPE_NAMES one short "
            "(kOrderTypeNameCount - 1), so flox.ORDER_TYPE_NAMES silently drops \"iceberg\" "
            "even though order_type_name(7)/order_type_code(\"iceberg\") still work",
        kind="py",
        edits=[Edit(
            file=ORDER_TYPE_BINDINGS_H,
            old="""  py::tuple names(flox::capi::kOrderTypeNameCount);
  for (std::size_t i = 0; i < flox::capi::kOrderTypeNameCount; ++i)
  {
    names[i] = flox::capi::kOrderTypeNamesLower[i];
  }""",
            new="""  py::tuple names(flox::capi::kOrderTypeNameCount - 1);
  for (std::size_t i = 0; i < flox::capi::kOrderTypeNameCount - 1; ++i)
  {
    names[i] = flox::capi::kOrderTypeNamesLower[i];
  }""",
        )],
        py_tests=["python/tests/test_order_type_names.py"],
        sweep_rebuild_py=True,
    ),

    # ═══════════════════════════════════════════════════════════════════
    # 2. ABI version handshake
    # ═══════════════════════════════════════════════════════════════════

    Mutation(
        name="abi-check-accepts-mismatch",
        why="checkAbiVersion's comparison is replaced with `if (true)`, so it always reports "
            "a match and clears the message regardless of what was passed in",
        kind="gtest",
        edits=[Edit(
            file=ABI_CHECK_HPP,
            old="""  const uint32_t runtimeVersion = flox_capi_abi_version();
  if (compiledVersion == runtimeVersion)
  {""",
            new="""  const uint32_t runtimeVersion = flox_capi_abi_version();
  if (true)
  {""",
        )],
        gtest_target="test_capi_abi_check",
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_quickjs"],
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="abi-check-mismatch-returns-true-with-message",
        why="the comparison is untouched, but the terminal `return false;` on a real mismatch "
            "becomes `return true;` -- the message still names both numbers, the verdict lies",
        kind="gtest",
        edits=[Edit(
            file=ABI_CHECK_HPP,
            old="""               "library it loads, or install the library the binding was built for.";
  }
  return false;
}""",
            new="""               "library it loads, or install the library the binding was built for.";
  }
  return true;
}""",
        )],
        gtest_target="test_capi_abi_check",
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_quickjs"],
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="abi-check-constant-instead-of-runtime-call",
        why="runtimeVersion is read from the FLOX_CAPI_ABI_VERSION macro instead of calling "
            "flox_capi_abi_version() -- indistinguishable from the correct code whenever the "
            "binding and the library it links were built from the same header, which is every "
            "case this single-tree test harness can construct",
        kind="gtest",
        edits=[Edit(
            file=ABI_CHECK_HPP,
            old="  const uint32_t runtimeVersion = flox_capi_abi_version();",
            new="  const uint32_t runtimeVersion = FLOX_CAPI_ABI_VERSION;",
        )],
        gtest_target="test_capi_abi_check",
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_quickjs"],
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="abi-check-message-swaps-compiled-and-runtime",
        why="the refusal message's two std::to_string(...) arguments are swapped, so it labels "
            "the compiled-against version as the library's and vice versa -- both numbers are "
            "still present, and the test only checks presence, not which label they sit under",
        kind="gtest",
        edits=[Edit(
            file=ABI_CHECK_HPP,
            old="""    *message = "flox C ABI version mismatch: this binding was built against version " +
               std::to_string(compiledVersion) + ", the loaded flox library reports version " +
               std::to_string(runtimeVersion) +""",
            new="""    *message = "flox C ABI version mismatch: this binding was built against version " +
               std::to_string(runtimeVersion) + ", the loaded flox library reports version " +
               std::to_string(compiledVersion) +""",
        )],
        gtest_target="test_capi_abi_check",
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_quickjs"],
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="abi-python-import-skips-check",
        why="the throw-on-mismatch block is removed from the _flox_py module init entirely; "
            "CAPI_ABI_VERSION and capi_abi_version() are still bound from the same macro/"
            "function as before, so their equality (all test_abi_version.py checks) holds "
            "regardless of whether the check ever ran",
        kind="py",
        edits=[Edit(
            file=FLOX_PY_CPP,
            old="""  {
    std::string abiMessage;
    if (!flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, &abiMessage))
    {
      throw py::import_error(abiMessage);
    }
  }
  m.attr("CAPI_ABI_VERSION") = static_cast<uint32_t>(FLOX_CAPI_ABI_VERSION);""",
            new="""  m.attr("CAPI_ABI_VERSION") = static_cast<uint32_t>(FLOX_CAPI_ABI_VERSION);""",
        )],
        py_tests=["python/tests/test_abi_version.py"],
        sweep_rebuild_py=True,
    ),
    Mutation(
        name="abi-node-exports-runtime-as-compiled",
        why="CAPI_ABI_VERSION is exported from flox_capi_abi_version() (the runtime read) "
            "instead of the FLOX_CAPI_ABI_VERSION macro (the compiled-against constant) -- the "
            "two are equal in any single build tree, so test_abi_version.js's equality check "
            "cannot tell the difference",
        kind="node",
        edits=[Edit(
            file=NODE_ABI_H,
            old="""  exports.Set("CAPI_ABI_VERSION",
              Napi::Number::New(env, static_cast<double>(FLOX_CAPI_ABI_VERSION)));""",
            new="""  exports.Set("CAPI_ABI_VERSION",
              Napi::Number::New(env, static_cast<double>(flox_capi_abi_version())));""",
        )],
        node_test="test_abi_version.js",
        sweep_rebuild_node=True,
    ),
    Mutation(
        name="abi-quickjs-registers-despite-refusal",
        why="registerFloxBindings computes the refusal message but no longer throws or returns "
            "false on a mismatch -- it registers every global regardless. In this build tree "
            "the compiled-against and runtime versions are always equal (one compiler, one "
            "header), so the refusal path this deletes never actually executes today; nothing "
            "in test_quickjs.cpp or elsewhere constructs a real header/library skew to reach it",
        kind="gtest",
        edits=[Edit(
            file=JS_BINDINGS_CPP,
            old="""  {
    std::string abiMessage;
    if (!capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, &abiMessage))
    {
      JS_ThrowInternalError(ctx, "%s", abiMessage.c_str());
      return false;
    }
  }""",
            new="""  {
    std::string abiMessage;
    capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, &abiMessage);
    // BUG: the refusal is computed and ignored; registration proceeds anyway.
  }""",
        )],
        gtest_target="test_quickjs",
        # Both ABI tests: the pair-is-exposed one cannot see this, the
        # refusal one can -- it drives registerFloxBindings against a
        # library reporting another version (tests/test_quickjs.cpp
        # defines flox_capi_abi_version itself) and asserts nothing is
        # registered.
        gtest_filter="JsEngineTest.*Abi*",
    ),

    # ═══════════════════════════════════════════════════════════════════
    # 3. VenueExecutor trigger (bare_executor_bindings.h)
    # ═══════════════════════════════════════════════════════════════════

    Mutation(
        name="venue-executor-trigger-not-set",
        why="the isConditional block that arms Order::triggerPrice is deleted outright -- "
            "every stop/take-profit submitted through VenueExecutor carries a zero trigger "
            "again and fires on the first print, the original finding this fix corrected",
        kind="py",
        edits=[Edit(
            file=BARE_EXECUTOR_BINDINGS_H,
            old="""            // The simulator arms a conditional order off
            // Order::triggerPrice, which this binding never set: every
            // stop and take-profit submitted here carried a zero trigger
            // and fired on the first print. Same rule as the standalone
            // SimulatedExecutor binding -- `price` doubles as the trigger
            // for the market-style conditionals when the caller left
            // `trigger` unset, and the limit-style pair needs both.
            const bool isConditional = order.type == OrderType::STOP_MARKET ||
                                       order.type == OrderType::STOP_LIMIT ||
                                       order.type == OrderType::TAKE_PROFIT_MARKET ||
                                       order.type == OrderType::TAKE_PROFIT_LIMIT;
            if (isConditional)
            {
              order.triggerPrice =
                  Price::fromDouble(trigger > 0.0 ? trigger : price);
            }
            self.submitOrder(order);""",
            new="""            self.submitOrder(order);""",
        )],
        py_tests=["python/tests/test_venue_executor_trigger.py"],
        sweep_rebuild_py=True,
    ),
    Mutation(
        name="venue-executor-trigger-from-limit-price-only",
        why="the isConditional arming stays, but it always uses `price` and ignores the "
            "explicit `trigger` argument -- a tp_limit submitted with a distinct trigger and "
            "limit price arms at the wrong level",
        kind="py",
        edits=[Edit(
            file=BARE_EXECUTOR_BINDINGS_H,
            old="""              order.triggerPrice =
                  Price::fromDouble(trigger > 0.0 ? trigger : price);""",
            new="""              order.triggerPrice = Price::fromDouble(price);""",
        )],
        py_tests=["python/tests/test_venue_executor_trigger.py"],
        sweep_rebuild_py=True,
    ),

    # ═══════════════════════════════════════════════════════════════════
    # 4. simulated_executor.cpp -- fillsAsMarket/fillsAsLimit, and the
    # rewrite triggerConditionalOrder no longer does
    # ═══════════════════════════════════════════════════════════════════

    Mutation(
        name="simulated-executor-trigger-rewrites-type-again",
        why="triggerConditionalOrder goes back to overwriting order.type to MARKET/LIMIT on "
            "fire, so a fired take-profit's fill/order-update events report \"market\"/\"limit\" "
            "again instead of the canonical \"tp_market\"/\"tp_limit\" the strategy actually "
            "placed -- the defect the whole task traces back to",
        kind="py",
        edits=[Edit(
            file=SIMULATED_EXECUTOR_CPP,
            old="""  const int64_t triggerBoundRaw = order.triggerPrice.raw();

  // A triggered conditional order enters the book now, so it is the aggressor
  // if it crosses.""",
            new="""  const int64_t triggerBoundRaw = order.triggerPrice.raw();

  if (order.type == OrderType::STOP_MARKET || order.type == OrderType::TAKE_PROFIT_MARKET ||
      order.type == OrderType::TRAILING_STOP)
  {
    order.type = OrderType::MARKET;
  }
  else if (order.type == OrderType::STOP_LIMIT || order.type == OrderType::TAKE_PROFIT_LIMIT)
  {
    order.type = OrderType::LIMIT;
  }

  // A triggered conditional order enters the book now, so it is the aggressor
  // if it crosses.""",
        )],
        py_tests=["python/tests/test_order_type_names.py", "python/tests/test_conditional_orders.py"],
        extra_py_objects=["CMakeFiles/flox.dir/src/backtest/simulated_executor.cpp.o"],
        sweep_rebuild_py=True,
    ),
    Mutation(
        name="simulated-executor-fillsasmarket-false-for-tp-market",
        why="fillsAsMarket drops TAKE_PROFIT_MARKET from its OR chain, so a fired take-profit "
            "walks the fill path meant for the limit-style pair instead of the market-style trio",
        kind="py",
        edits=[Edit(
            file=SIMULATED_EXECUTOR_CPP,
            old="""bool fillsAsMarket(OrderType type)
{
  return type == OrderType::MARKET || type == OrderType::STOP_MARKET ||
         type == OrderType::TAKE_PROFIT_MARKET || type == OrderType::TRAILING_STOP;
}""",
            new="""bool fillsAsMarket(OrderType type)
{
  return type == OrderType::MARKET || type == OrderType::STOP_MARKET ||
         type == OrderType::TRAILING_STOP;
}""",
        )],
        py_tests=["python/tests/test_order_type_names.py", "python/tests/test_conditional_orders.py"],
        extra_py_objects=["CMakeFiles/flox.dir/src/backtest/simulated_executor.cpp.o"],
        sweep_rebuild_py=True,
    ),
    Mutation(
        name="simulated-executor-fillsaslimit-false-for-tp-limit",
        why="fillsAsLimit drops TAKE_PROFIT_LIMIT, so a fired tp_limit no longer rests/rejects "
            "like the limit-style pair the fix groups it with",
        kind="py",
        edits=[Edit(
            file=SIMULATED_EXECUTOR_CPP,
            old="""bool fillsAsLimit(OrderType type)
{
  return type == OrderType::LIMIT || type == OrderType::STOP_LIMIT ||
         type == OrderType::TAKE_PROFIT_LIMIT;
}""",
            new="""bool fillsAsLimit(OrderType type)
{
  return type == OrderType::LIMIT || type == OrderType::STOP_LIMIT;
}""",
        )],
        py_tests=["python/tests/test_order_type_names.py", "python/tests/test_conditional_orders.py"],
        extra_py_objects=["CMakeFiles/flox.dir/src/backtest/simulated_executor.cpp.o"],
        sweep_rebuild_py=True,
        equivalent_reason=(
            "not observable from any binding, checked rather than assumed. fillsAsLimit "
            "has three call sites and none of them decides whether a take-profit limit "
            "fills: the fill paths are gated on !fillsAsMarket, so a fired tp_limit rests "
            "and fills identically with and without this edit -- measured, with a book "
            "(TakeProfitLimitTests), with a queue model and bar steps, and against a plain "
            "limit and a stop limit as controls, all four identical. What the three call "
            "sites do gate is driveQueueFromBarStep, the queue tracker's registration, and "
            "maybeEmitMarketPositionChanges -- event streams, not fills. Those are emitted "
            "from onBookSnapshot/onTrade, and no binding can drive a book-fed "
            "SimulatedExecutor from a strategy: Runner.set_executor and "
            "BacktestRunner.set_executor take the hook-style Executor or a VenueExecutor, "
            "never a SimulatedExecutor, and VenueExecutor exposes no set_queue_model. "
            "Separately, and worth its own task: on unmutated code a resting *triggered* "
            "take-profit limit already emits no market-position event where an identical "
            "plain limit emits one (probe: BacktestRunner.run_tape over a tape with book "
            "snapshots, on_market_position_change fires for limit_sell, never for "
            "take_profit_limit), so the event stream this mutation would break is not "
            "reaching take-profit limits today either"
        ),
    ),

    # ═══════════════════════════════════════════════════════════════════
    # 5. Ownership notes (flox_capi.h) -- read by test_capi_contract as a
    # text file, so the "rebuild" is nominal; it still forces a real
    # recompile because the target's own object is deleted first.
    # ═══════════════════════════════════════════════════════════════════

    Mutation(
        name="ownership-note-removed-curve-clone",
        why="flox_curve_clone's owned note is deleted outright -- back to undocumented, one "
            "of the six the audit named",
        kind="gtest",
        edits=[Edit(
            file=FLOX_CAPI_H,
            old="""  /* Owned: a fresh copy the caller destroys with flox_curve_destroy,
   * independent of the curve it was cloned from. */
  FloxCurveHandle flox_curve_clone(FloxCurveHandle curve);""",
            new="""  FloxCurveHandle flox_curve_clone(FloxCurveHandle curve);""",
        )],
        gtest_target="test_capi_contract",
        gtest_filter="CapiContractTest.*Ownership*",
    ),
    Mutation(
        name="ownership-note-vague-neither-owned-nor-borrowed",
        why="flox_curve_clone's note is replaced with wording that never says owned/owns/"
            "borrow -- mentionsOwnership() is a keyword match, so language that only implies "
            "the rule without using its words still counts as undocumented",
        kind="gtest",
        edits=[Edit(
            file=FLOX_CAPI_H,
            old="""  /* Owned: a fresh copy the caller destroys with flox_curve_destroy,
   * independent of the curve it was cloned from. */
  FloxCurveHandle flox_curve_clone(FloxCurveHandle curve);""",
            new="""  /* The caller is responsible for eventually calling flox_curve_destroy
   * on the value this returns. */
  FloxCurveHandle flox_curve_clone(FloxCurveHandle curve);""",
        )],
        gtest_target="test_capi_contract",
        gtest_filter="CapiContractTest.*Ownership*",
    ),
    Mutation(
        name="ownership-note-wrong-borrowed-marked-owned",
        why="flox_pool_replay_curve is genuinely BORROWED (flox_curve_destroy on it is a "
            "no-op) but its note is rewritten to say \"Owned\" and tell the caller to destroy "
            "it -- a caller who follows this comment double-frees the replay's own curve. "
            "mentionsOwnership() only checks that one of owned/owns/borrow appears somewhere "
            "near the declaration; it cannot tell which one is true",
        kind="gtest",
        edits=[Edit(
            file=FLOX_CAPI_H,
            old="""  /* Borrowed: the replay owns this curve. It stays valid while the replay
   * lives and flox_curve_destroy on it is a no-op. */
  FloxCurveHandle flox_pool_replay_curve(FloxPoolReplayHandle replay);""",
            new="""  /* Owned: the replay hands out a fresh copy of its curve here. Destroy
   * it with flox_curve_destroy when done with it. */
  FloxCurveHandle flox_pool_replay_curve(FloxPoolReplayHandle replay);""",
        )],
        gtest_target="test_capi_contract",
        gtest_filter="CapiContractTest.*Ownership*",
    ),

    # ═══════════════════════════════════════════════════════════════════
    # 6. bundle.py -- pure-Python source, no C++ rebuild, but _flox_py's
    # POST_BUILD copy_directory step still has to refresh it, so this goes
    # through the same "py" rebuild pipeline as everything else.
    # ═══════════════════════════════════════════════════════════════════

    Mutation(
        name="bundle-translates-tp-market-to-long-name",
        why="_run_strategy_against_tape's on_signal reintroduces a translation from the "
            "canonical short names to the pre-fix long ones before calling submit_order, so a "
            "strategy that emits a tp_market/tp_limit Signal through pack_bundle()/replay_bundle() "
            "hits the ValueError submit_order now raises for \"take_profit_market\". No "
            "strategy fixture in python/tests/test_bundle.py (or anywhere else that drives "
            "_run_strategy_against_tape) ever emits a conditional-order signal -- every bundled "
            "test strategy only calls market_buy() -- so this path is unexercised",
        kind="py",
        edits=[
            Edit(
                file=BUNDLE_PY,
                old="""    _TRACE_ORDER_TYPE = {
        name: code for code, name in enumerate(_EXEC_ORDER_TYPES)
    }""",
                new="""    _TRACE_ORDER_TYPE = {
        name: code for code, name in enumerate(_EXEC_ORDER_TYPES)
    }
    # BUG: reintroduces the translation this fix removed -- tp_market/
    # tp_limit are turned back into the long names submit_order no longer
    # accepts, so a conditional-order signal now raises ValueError instead
    # of reaching the book.
    _LEGACY_EXEC_TYPE = {
        "tp_market": "take_profit_market",
        "tp_limit": "take_profit_limit",
    }""",
            ),
            Edit(
                file=BUNDLE_PY,
                old="""        sim.submit_order(
            oid, side, price, qty,
            type=order_type, symbol=int(sym),""",
                new="""        sim.submit_order(
            oid, side, price, qty,
            type=_LEGACY_EXEC_TYPE.get(order_type, order_type), symbol=int(sym),""",
            ),
        ],
        # BundleConditionalOrderTests drives _run_strategy_against_tape with a
        # strategy that emits a take-profit signal, and pack_bundle/replay_bundle
        # over the same pair -- the path every other bundled fixture misses by
        # only ever calling market_buy().
        py_tests=["python/tests/test_bundle.py"],
        sweep_rebuild_py=True,
    ),

    # ═══════════════════════════════════════════════════════════════════
    # 7. Not covered by any binding test: trace_handlers.h's fourth,
    # deliberately-untouched order-type table (.floxrun trace format).
    # ═══════════════════════════════════════════════════════════════════

    Mutation(
        name="trace-handlers-tp-market-tp-limit-swapped",
        why="TraceHandlers::signalTypeName (include/flox/run/trace_handlers.h, the .floxrun "
            "trace writer's own copy of the order-type table -- explicitly out of scope for "
            "this task per the task note) has TakeProfitMarket and TakeProfitLimit's strings "
            "swapped. tests/test_trace_handlers.cpp is the only file that even includes this "
            "header, and it never asserts on signalTypeName's order-type strings at all -- so "
            "this is expected to survive against every one of the four required surfaces AND "
            "against its own dedicated test file",
        kind="gtest",
        edits=[Edit(
            file=TRACE_HANDLERS_H,
            old="""      case SignalType::TakeProfitMarket:
        return "take_profit_market";
      case SignalType::TakeProfitLimit:
        return "take_profit_limit";""",
            new="""      case SignalType::TakeProfitMarket:
        return "take_profit_limit";
      case SignalType::TakeProfitLimit:
        return "take_profit_market";""",
        )],
        gtest_target="test_trace_handlers",
        sweep_rebuild_py=True,
        sweep_rebuild_gtest_targets=["test_quickjs"],
        sweep_rebuild_node=True,
        equivalent_reason=(
            "out of scope, recorded as a follow-up: trace_handlers.h is the .floxrun "
            "trace writer's own table, not a language binding. Its strings are a "
            "recorded file format, so changing them is a reader-compatibility "
            "question this task did not open -- the task note says so explicitly. "
            "Pinning the writer's names here would freeze the legacy spelling in a "
            "test and make the follow-up harder, so this mutation is left alive "
            "deliberately rather than covered"
        ),
    ),
]

# ── Not executed: Codon ─────────────────────────────────────────────────
#
# The task asks for "one Codon module without the abi import (say whether
# any test can see it)" -- e.g. dropping the `from flox.abi import
# check_abi_version` line codon/flox/backtest.codon (or any of the other 21
# modules) carries alongside `from C import ...`.
#
# This is deliberately NOT in MUTATIONS above -- untestable as a mutation
# here, for a stated reason: there is no `codon` compiler on this machine
# (`which codon` finds nothing), and the harness's own rule is "a mutation
# that does not compile is not a mutation" -- there is no way here to
# prove the mutated module still builds, so it cannot be run honestly.
#
# What it can have instead is a test that reads the modules, which is what
# python/tests/test_abi_version.py::
# test_every_codon_module_with_c_declarations_runs_the_abi_check now does:
# every codon/flox/*.codon that carries a `from C import` must also carry
# `from flox.abi import check_abi_version`, the import whose side effect
# is the check (abi.codon calls check_abi_version() at module level). Drop
# that line from any one module and the test names the module. It is a
# text-level pin, not a behavioural one -- it cannot tell that the check
# still runs, only that the module still asks for it -- and it is what is
# available without a toolchain.
#
# Answering "which test would catch it" as of the first pass, from reading
# rather than running: none would. `tests/test_capi_abi_check.cpp` is C++-only.
# `scripts/check_codon_examples_coverage.py` (the one Codon-aware CI gate
# that inspects source rather than running a binary) audits
# codon/examples/*.codon against what CI executes -- it never reads
# codon/flox/*.codon, so a missing `from flox.abi import check_abi_version`
# in one of the 22 library modules is invisible to it. The only thing that
# would ever exercise the missing import is CI's own "Run Codon smoke
# examples" step actually importing that specific module at runtime and
# hitting the ABI mismatch this handshake exists to catch -- which requires
# a mismatched libflox_capi to begin with, the same gap
# abi-python-import-skips-check / abi-quickjs-registers-despite-refusal
# document for the other three bindings.


# ═════════════════════════════════════════════════════════════════════════
# build / test plumbing
# ═════════════════════════════════════════════════════════════════════════

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


def find_objects(build_dir: Path, name_glob: str) -> list[Path]:
    out = subprocess.run(
        ["find", str(build_dir), "-type", "f", "-name", "*.o", "-path", f"*{name_glob}*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, output: str):
        super().__init__("rebuild failed")
        self.output = output


def cmake_build(build_dir: Path, targets: list[str], require_cxx: bool = True) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(build_dir), "--target", *targets, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(output)
    if require_cxx and "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {targets} in {build_dir} compiled nothing -- the result would have "
            f"been stale, so the run is refused:\n{output[-2000:]}"
        )
    return output


def rebuild_py(extra_objects: list[str], require_cxx: bool = True) -> str:
    for obj in find_objects(BUILD_PY / "python", "_flox_py.dir"):
        obj.unlink()
    for rel in extra_objects:
        p = BUILD_PY / rel
        if p.exists():
            p.unlink()
    return cmake_build(BUILD_PY, [PY_TARGET], require_cxx=require_cxx)


def rebuild_gtest(targets: list[str], extra_objects: list[str], require_cxx: bool = True) -> str:
    for t in targets:
        for obj in find_objects(BUILD / "tests", f"{t}.dir"):
            obj.unlink()
    for rel in extra_objects:
        p = BUILD / rel
        if p.exists():
            p.unlink()
    return cmake_build(BUILD, targets, require_cxx=require_cxx)


def node_addon_stat() -> tuple[int, float] | None:
    if not NODE_ADDON.exists():
        return None
    st = NODE_ADDON.stat()
    return (st.st_size, st.st_mtime)


def rebuild_node() -> str:
    before = node_addon_stat()
    result = subprocess.run(
        [NPM, "run", "build"], capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=NODE_DIR,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(output)
    after = node_addon_stat()
    if after is None:
        raise SystemExit(f"node rebuild reported success but {NODE_ADDON} does not exist")
    print(f"    node addon  before {before}  after {after}")
    return output


def run_pytest(paths: list[str], timeout: int) -> tuple[int, str]:
    env = dict(os.environ)
    env["PYTHONPATH"] = PYTHONPATH
    cmd = [PYTHON, "-m", "pytest", *paths, "-q"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO, env=env)
    except subprocess.TimeoutExpired as expired:
        out = expired.stdout or ""
        err = expired.stderr or ""
        return 124, f"timed out after {timeout}s\n{out}{err}"
    return result.returncode, result.stdout + result.stderr


def run_gtest_binary(target: str, gtest_filter: str | None, timeout: int) -> tuple[int, str]:
    binary = BUILD / "tests" / target
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout}s"
    return result.returncode, result.stdout + result.stderr


def run_all_capi_binaries(timeout: int) -> tuple[str | None, int, str]:
    """Every build/tests/test_capi_* binary, no rebuild (only the ones this
    mutation's sweep explicitly rebuilt are fresh; the rest are unaffected
    by construction and running the existing binary is the honest check).
    Returns the first one that fails, or (None, 0, '')."""
    for b in sorted((BUILD / "tests").glob("test_capi_*")):
        if not (b.is_file() and os.access(b, os.X_OK)):
            continue
        try:
            result = subprocess.run([str(b)], capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return b.name, 124, f"timed out after {timeout}s"
        if result.returncode != 0:
            return b.name, result.returncode, result.stdout + result.stderr
    return None, 0, ""


def run_quickjs_binary(timeout: int) -> tuple[int, str]:
    return run_gtest_binary("test_quickjs", None, timeout)


def run_node_test(name: str, timeout: int) -> tuple[int, str]:
    try:
        result = subprocess.run(["node", f"test/{name}"], capture_output=True, text=True,
                                timeout=timeout, cwd=NODE_DIR)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout}s"
    return result.returncode, result.stdout + result.stderr


def run_node_suite(timeout_per_file: int) -> tuple[str | None, int, str]:
    for f in sorted((NODE_DIR / "test").glob("test_*.js")):
        code, output = run_node_test(f.name, timeout_per_file)
        if code != 0:
            return f.name, code, output
    return None, 0, ""


def summary_line(output: str) -> str:
    tail = [line for line in output.splitlines() if line.strip()]
    return tail[-1].strip() if tail else ""


# ═════════════════════════════════════════════════════════════════════════
# mutation driver
# ═════════════════════════════════════════════════════════════════════════

def apply_edits(m: Mutation) -> dict[str, tuple[str, str]]:
    """Writes the mutated files; returns {file: (original, before_hash)}."""
    paths = {e.file: REPO / e.file for e in m.edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    texts = dict(originals)
    for e in m.edits:
        texts[e.file] = replace_occurrence(texts[e.file], e.old, e.new, e.occurrence,
                                           e.expected_occurrences)
    for f, p in paths.items():
        if texts[f] == originals[f]:
            raise SystemExit(f"mutation changed nothing in {f}")
        p.write_text(texts[f])
    return {f: (originals[f], before[f]) for f in paths}


def restore_edits(m: Mutation, saved: dict[str, tuple[str, str]]) -> None:
    for f, (original, before_hash) in saved.items():
        p = REPO / f
        p.write_text(original)
        after = sha256(p)
        if after != before_hash:
            raise SystemExit(f"restore failed: {f} does not hash back to its original")


def primary_check(m: Mutation) -> tuple[bool, str]:
    """(compiled, detail). Raises SystemExit only for a hard harness error;
    a normal compile failure returns (False, ...)."""
    if m.kind == "py":
        try:
            output = rebuild_py(m.extra_py_objects)
        except BuildFailed as e:
            print("    DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return False, "no-compile"
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"    rebuilt {PY_TARGET}: {compiled} 'Building CXX' line(s)")
    elif m.kind == "gtest":
        try:
            output = rebuild_gtest([m.gtest_target], m.extra_gtest_objects)
        except BuildFailed as e:
            print("    DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return False, "no-compile"
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"    rebuilt {m.gtest_target}: {compiled} 'Building CXX' line(s)")
    elif m.kind == "node":
        try:
            rebuild_node()
        except BuildFailed as e:
            print("    DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return False, "no-compile"
    else:
        raise SystemExit(f"unknown kind {m.kind!r}")
    return True, ""


def run_primary_test(m: Mutation) -> tuple[bool, str]:
    """(red, description)."""
    if m.kind == "py":
        code, output = run_pytest(m.py_tests, TEST_TIMEOUT)
        desc = ", ".join(m.py_tests)
        print(f"    {desc} -> exit {code} ({'RED, killed' if code else 'green'})   "
              f"{summary_line(output)}")
        if code:
            failed = [line for line in output.splitlines()
                      if line.startswith("FAILED ") or line.startswith("ERROR ")]
            for line in failed[:10]:
                print(f"      {line}")
        return code != 0, desc
    if m.kind == "gtest":
        code, output = run_gtest_binary(m.gtest_target, m.gtest_filter, TEST_TIMEOUT)
        shown = m.gtest_filter or "(whole binary)"
        desc = f"{m.gtest_target} --gtest_filter={shown}"
        print(f"    {desc} -> exit {code} ({'RED, killed' if code else 'green'})")
        if code == 0 and m.gtest_filter:
            # confirm the filter matched something real
            if "0 tests from" in output or "Not Found" in output:
                raise SystemExit(f"gtest filter matched nothing:\n{output[-1000:]}")
        if code:
            print("\n".join(output.splitlines()[-15:]))
        return code != 0, desc
    if m.kind == "node":
        code, output = run_node_test(m.node_test, TEST_TIMEOUT)
        desc = f"node test/{m.node_test}"
        print(f"    {desc} -> exit {code} ({'RED, killed' if code else 'green'})")
        if code:
            print("\n".join(output.splitlines()[-15:]))
        return code != 0, desc
    raise SystemExit(f"unknown kind {m.kind!r}")


def sweep(m: Mutation) -> tuple[str | None, str]:
    """Runs the full named surface. Returns (killer_description, detail) or
    (None, '') if everything stayed green."""
    # Rebuild whichever other trees this mutation's file(s) actually reach.
    if m.sweep_rebuild_py and m.kind != "py":
        try:
            rebuild_py(m.extra_py_objects, require_cxx=False)
        except BuildFailed as e:
            return "build-py sweep rebuild", e.output[-2000:]
    if m.sweep_rebuild_gtest_targets:
        try:
            rebuild_gtest(m.sweep_rebuild_gtest_targets, [], require_cxx=False)
        except BuildFailed as e:
            return f"gtest sweep rebuild {m.sweep_rebuild_gtest_targets}", e.output[-2000:]
    if m.sweep_rebuild_node and m.kind != "node":
        try:
            rebuild_node()
        except BuildFailed as e:
            return "node sweep rebuild", e.output[-2000:]

    # 1. python/tests, full directory.
    code, output = run_pytest(["python/tests"], SWEEP_PY_TIMEOUT)
    print(f"    sweep  python/tests            -> exit {code} ({'RED' if code else 'green'})   "
          f"{summary_line(output)}")
    if code != 0:
        failed = [line for line in output.splitlines()
                  if line.startswith("FAILED ") or line.startswith("ERROR ")]
        return "python/tests (full)", "\n".join(failed[:15]) or output[-1500:]

    # 2. every build/tests/test_capi_* binary.
    killer, code, output = run_all_capi_binaries(TEST_TIMEOUT)
    print(f"    sweep  build/tests/test_capi_* -> {'RED: ' + killer if killer else 'all green'}")
    if killer:
        return f"test_capi_*/{killer}", output[-1500:]

    # 3. test_quickjs, whole binary.
    code, output = run_quickjs_binary(TEST_TIMEOUT)
    print(f"    sweep  test_quickjs            -> exit {code} ({'RED' if code else 'green'})")
    if code != 0:
        return "test_quickjs (whole binary)", output[-1500:]

    # 4. the Node suite, every test_*.js file.
    killer, code, output = run_node_suite(SWEEP_NODE_TIMEOUT)
    print(f"    sweep  node test suite         -> {'RED: ' + killer if killer else 'all green'}")
    if killer:
        return f"node/test/{killer}", output[-1500:]

    return None, ""


def run_mutation(m: Mutation, do_sweep: bool) -> tuple[str, str]:
    """(verdict, detail). verdict in killed/killed-elsewhere/alive/equivalent/no-compile."""
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    for f in m.files():
        print(f"  file    {f}")
    hashes_before = {f: sha256(REPO / f) for f in m.files()}
    for f, h in hashes_before.items():
        print(f"  sha256  before  {h}  {f}")

    saved = apply_edits(m)
    for f in m.files():
        print(f"  sha256  mutated {sha256(REPO / f)}  {f}")

    verdict = "alive"
    detail = ""
    try:
        compiled, why_not = primary_check(m)
        if not compiled:
            return "no-compile", why_not

        red, desc = run_primary_test(m)
        if red:
            return "killed", desc

        if not do_sweep:
            print("    primary green, --skip-sweep set -- treating as alive for this run")
            return ("equivalent" if m.equivalent_reason else "alive"), m.equivalent_reason

        killer, detail = sweep(m)
        if killer:
            return "killed-elsewhere", killer

        if m.equivalent_reason:
            print(f"  EQUIVALENT: {m.equivalent_reason}")
            return "equivalent", m.equivalent_reason
        print("  GREEN EVERYWHERE -- a hole")
        return "alive", ""
    finally:
        restore_edits(m, saved)
        for f in m.files():
            after = sha256(REPO / f)
            print(f"  sha256  after   {after}  {f}")
        # Bring every tree this mutation's primary build or sweep touched
        # back in sync with the now-restored source, so the NEXT mutation
        # does not run against a stale, still-mutated binary.
        needs_py = m.kind == "py" or m.sweep_rebuild_py
        needs_gtest = ([m.gtest_target] if m.kind == "gtest" else []) + m.sweep_rebuild_gtest_targets
        needs_node = m.kind == "node" or m.sweep_rebuild_node
        try:
            if needs_py:
                rebuild_py(m.extra_py_objects, require_cxx=False)
            if needs_gtest:
                rebuild_gtest(sorted(set(needs_gtest)), m.extra_gtest_objects, require_cxx=False)
            if needs_node:
                rebuild_node()
        except BuildFailed as e:
            raise SystemExit(
                f"post-mutation restore-rebuild failed for {m.name} -- the tree is left in an "
                f"inconsistent state:\n{e.output[-3000:]}"
            )


def control() -> bool:
    print("control run before the mutations")
    ok = True
    try:
        rebuild_py([])
    except BuildFailed as e:
        print(e.output[-2000:])
        return False
    code, output = run_pytest(["python/tests/test_order_type_names.py",
                               "python/tests/test_conditional_orders.py",
                               "python/tests/test_abi_version.py",
                               "python/tests/test_venue_executor_trigger.py"], TEST_TIMEOUT)
    print(f"  control python (4 relevant files) -> exit {code}   {summary_line(output)}")
    ok = ok and code == 0

    for target, gfilter in [("test_capi_abi_check", None), ("test_capi_contract", None),
                            ("test_quickjs", None), ("test_trace_handlers", None)]:
        rebuild_gtest([target], [])
        code, output = run_gtest_binary(target, gfilter, TEST_TIMEOUT)
        print(f"  control {target:<24} -> exit {code}   "
              f"{next((l for l in output.splitlines() if l.startswith('[  PASSED  ]')), '')}")
        ok = ok and code == 0

    rebuild_node()
    code, output = run_node_test("test_abi_version.js", TEST_TIMEOUT)
    print(f"  control node test_abi_version.js -> exit {code}")
    ok = ok and code == 0

    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--skip-control", action="store_true")
    parser.add_argument("--skip-sweep", action="store_true",
                        help="primary check only; do not sweep survivors (fast iteration)")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<52} {m.kind:<6} {', '.join(m.files())}")
        return 0

    for d, label in [(BUILD, "build"), (BUILD_PY, "build-py")]:
        if not (d / "CMakeCache.txt").is_file():
            raise SystemExit(f"{d} is not configured (see module docstring for the {label} setup)")
    if not NODE_ADDON.exists():
        raise SystemExit(f"{NODE_ADDON} does not exist; run `npm install && npm run build` in node/")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    if not args.skip_control:
        if not control():
            raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m, do_sweep=not args.skip_sweep)) for m in selected]

    restored = True
    if not args.skip_control:
        print("\ncontrol run after the mutations")
        restored = control()

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
        print(f"\n{len(survived)} mutation(s) survived -- holes:")
        for name, _ in survived:
            print(f"  - {name}")
    if equivalent:
        print(f"\n{len(equivalent)} mutation(s) survived but are equivalent:")
        for name, reason in equivalent:
            print(f"  - {name}: {reason}")
    if killed_elsewhere:
        print(f"\n{len(killed_elsewhere)} mutation(s) were killed by the sweep, not the primary test:")
        for name, reason in killed_elsewhere:
            print(f"  - {name}: {reason}")
    if nobuild:
        print(f"\n{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")

    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
