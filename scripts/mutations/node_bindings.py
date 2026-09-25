#!/usr/bin/env python3
"""Mutation harness for the Node bindings BigInt boundary, the runBars/
runOhlcv length checks, the gate failure policy, and the removal of the
dead HookMode::Threaded half.

A test that passes proves nothing on its own; what it has to do is fail
when the code it covers is wrong. This script breaks each piece of the
fix one at a time, in node/src, and checks that the relevant Node test
goes red -- and that an unmutated addon is green before and after.

The whole addon is one translation unit (node/src/flox_node.cpp includes
every header), so every mutation here rebuilds the same object file:
node/build/CMakeFiles/flox_node.dir/src/flox_node.cpp.o. That object is
deleted before every rebuild so nothing is served from cache, the
cmake-js output has to contain "Building CXX object" naming that object
or the run is refused, and node/build/Release/flox_node.node's mtime and
size are printed before and after as a second, independent signal that a
real rebuild happened.

A mutation names one expected-red test file. That one runs first; if it
does not go red, the harness sweeps every node/test/test_*.js file (the
whole suite) before calling the mutation a survivor -- some regressions
here are caught by a file other than the one that documents them, and a
few are not caught anywhere at all, which is the point of running this.

Usage:

    python3 scripts/mutations/node_bindings.py                 # control, all mutations, control
    python3 scripts/mutations/node_bindings.py --list
    python3 scripts/mutations/node_bindings.py --only bars-to-js-back-to-number

Prerequisites (not done by this script):

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFLOX_BUILD_CAPI=ON
    cmake --build build
    cd node && npm install && npm run build
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NODE_DIR = REPO / "node"
SRC_DIR = NODE_DIR / "src"
TEST_DIR = NODE_DIR / "test"
BUILD_DIR = NODE_DIR / "build"
ADDON = BUILD_DIR / "Release" / "flox_node.node"
# node/src/flox_node.cpp #includes every header under node/src -- there is
# exactly one translation unit for the whole addon, so every mutation here
# recompiles this same object file.
OBJECT = BUILD_DIR / "CMakeFiles" / "flox_node.dir" / "src" / "flox_node.cpp.o"

NPM = os.environ.get("NPM", "npm")
BUILD_TIMEOUT = 300
TEST_TIMEOUT = 60
SWEEP_TIMEOUT = 30  # per test file during a whole-suite sweep


@dataclass
class Mutation:
    name: str
    why: str
    file: str  # path under node/src
    old: str
    new: str
    test: str | None = None  # path under node/test; None means whole-suite only
    occurrence: int = 1
    expected_occurrences: int = 1
    # Set True only when the survivor is genuinely unobservable from JS --
    # written with the reason in `why`. Every mutation below is a real
    # behaviour change, so none are marked equivalent; the field exists so
    # the harness schema supports it if a future mutation needs it.
    equivalent: bool = False


MUTATIONS: list[Mutation] = [
    # ── 1-3: bars back to Number, one builder at a time (finding 3) ────────
    Mutation(
        name="bars-to-js-back-to-number",
        why="aggregators.h barsToJs: startTimeNs/endTimeNs go back to (double) casts, "
            "so every aggregate*Bars() call quantises a real ns reading to 256 ns steps",
        file="aggregators.h",
        old='''    o.Set("startTimeNs", Napi::BigInt::New(env, static_cast<int64_t>(bars[i].start_time_ns)));
    o.Set("endTimeNs", Napi::BigInt::New(env, static_cast<int64_t>(bars[i].end_time_ns)));''',
        new='''    o.Set("startTimeNs", (double)bars[i].start_time_ns);
    o.Set("endTimeNs", (double)bars[i].end_time_ns);''',
        test="test_bar_timestamp_bigint.js",
    ),
    Mutation(
        name="build-bar-obj-back-to-number",
        why="strategy.h buildBarObj (used by NodeStrategyHost::onBar / callOnBar, i.e. the "
            "live bar delivered to a strategy's onBar handler) goes back to Number for "
            "startTimeNs/endTimeNs",
        file="strategy.h",
        old='''    // BigInt for the same reason as TradeData.timestampNs above: a real ns
    // reading does not survive a double.
    o.Set("startTimeNs", Napi::BigInt::New(env, static_cast<int64_t>(bar->start_time_ns)));
    o.Set("endTimeNs", Napi::BigInt::New(env, static_cast<int64_t>(bar->end_time_ns)));''',
        new='''    o.Set("startTimeNs", Napi::Number::New(env, static_cast<double>(bar->start_time_ns)));
    o.Set("endTimeNs", Napi::Number::New(env, static_cast<double>(bar->end_time_ns)));''',
        test="test_bar_timestamp_bigint.js",
    ),
    Mutation(
        name="build-bar-lambda-back-to-number",
        why="strategy.h's buildBar lambda (the multi-TF bar-ring accessors "
            "lastClosedBar()/lastNClosedBars()) goes back to Number for startNs/endNs. "
            "No node/test file calls either accessor at runtime -- only index.d.ts's "
            "declared ClosedBar.startNs/endNs are checked as text -- so this is expected "
            "to survive",
        file="strategy.h",
        old='''      o.Set("startNs",
            Napi::BigInt::New(env, static_cast<int64_t>(b.startTime.time_since_epoch().count())));
      o.Set("endNs",
            Napi::BigInt::New(env, static_cast<int64_t>(b.endTime.time_since_epoch().count())));''',
        new='''      o.Set("startNs",
            Napi::Number::New(env, static_cast<double>(b.startTime.time_since_epoch().count())));
      o.Set("endNs",
            Napi::Number::New(env, static_cast<double>(b.endTime.time_since_epoch().count())));''',
        test="test_bar_timestamp_bigint.js",
    ),

    # ── 4: BigInt64Array trade column reinterpreted as doubles again ───────
    Mutation(
        name="extract-trades-bigint-column-reinterpreted",
        why="aggregators.h extractTrades: disables the BigInt64Array branch, so a caller "
            "that passes real (BigInt64Array) trade timestamps falls through to "
            "info[0].As<Napi::Float64Array>(), reading the int64 bit pattern back as double "
            "garbage -- the exact bug the fix's own comment describes",
        file="aggregators.h",
        old="""  if (info[0].IsTypedArray() &&
      info[0].As<Napi::TypedArray>().TypedArrayType() == napi_bigint64_array)""",
        new="""  if (false && info[0].IsTypedArray() &&
      info[0].As<Napi::TypedArray>().TypedArrayType() == napi_bigint64_array)""",
        test="test_bar_timestamp_bigint.js",
    ),

    # ── 5: Runner.onBar dropping a BigInt to the default ────────────────────
    Mutation(
        name="runner-onbar-getint-drops-bigint",
        why="strategy.h's getInt lambda inside RunnerNode::onBar goes back to `IsNumber() "
            "? ... : dflt`, so a bar object built by this same addon (whose startTimeNs/"
            "endTimeNs are now BigInt) fed back into Runner.onBar() silently arrives "
            "stamped 0 instead of round-tripping",
        file="strategy.h",
        old="""    auto getInt = [&](const char* k, int64_t dflt) -> int64_t
    {
      auto v = opts.Get(k);
      if (v.IsBigInt() || v.IsNumber())
      {
        return toInt64Ns(v);
      }
      return dflt;
    };""",
        new="""    auto getInt = [&](const char* k, int64_t dflt) -> int64_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<int64_t>(v.As<Napi::Number>().Int64Value()) : dflt;
    };""",
        test="test_bar_timestamp_bigint.js",
    ),

    # ── 6-8: runBars / runOhlcv length checks (finding 4) ───────────────────
    Mutation(
        name="runbars-last-column-unchecked",
        why="strategy.h runBars: drops volA (the last of the seven columns) from the "
            "requireSameLength list, so a short `volume` array is no longer rejected "
            "before the C call",
        file="strategy.h",
        old="""      if (!requireSameLength(info.Env(), "runBars",
                             {startNs.ElementLength(), endNs.ElementLength(),
                              openA.ElementLength(), highA.ElementLength(),
                              lowA.ElementLength(), closeA.ElementLength(),
                              volA.ElementLength()}))""",
        new="""      if (!requireSameLength(info.Env(), "runBars",
                             {startNs.ElementLength(), endNs.ElementLength(),
                              openA.ElementLength(), highA.ElementLength(),
                              lowA.ElementLength(), closeA.ElementLength()}))""",
        test="test_run_bars_array_lengths.js",
    ),
    Mutation(
        name="runohlcv-unchecked",
        why="strategy.h runOhlcv: the requireSameLength guard is removed outright, so a "
            "short `close` or `ts` column is no longer rejected before the C call",
        file="strategy.h",
        old="""      auto tsArr = info[0].As<Napi::BigInt64Array>();
      auto closeArr = info[1].As<Napi::Float64Array>();
      if (!requireSameLength(info.Env(), "runOhlcv",
                             {tsArr.ElementLength(), closeArr.ElementLength()}))
      {
        return info.Env().Undefined();
      }
      std::string symbol = info[2].As<Napi::String>().Utf8Value();""",
        new="""      auto tsArr = info[0].As<Napi::BigInt64Array>();
      auto closeArr = info[1].As<Napi::Float64Array>();
      std::string symbol = info[2].As<Napi::String>().Utf8Value();""",
        test="test_run_bars_array_lengths.js",
    ),
    Mutation(
        name="runbars-check-moved-after-c-call",
        why="strategy.h runBars: the length check still exists but is moved to after "
            "flox_backtest_runner_run_bars() has already run over the mismatched "
            "columns -- 'nothing was dispatched on a mismatch' no longer holds",
        file="strategy.h",
        old="""      if (!requireSameLength(info.Env(), "runBars",
                             {startNs.ElementLength(), endNs.ElementLength(),
                              openA.ElementLength(), highA.ElementLength(),
                              lowA.ElementLength(), closeA.ElementLength(),
                              volA.ElementLength()}))
      {
        return info.Env().Undefined();
      }
      std::string symbol = info[7].As<Napi::String>().Utf8Value();
      uint8_t barType = info.Length() > 8 ? static_cast<uint8_t>(info[8].As<Napi::Number>().Uint32Value()) : 0;
      uint64_t barTypeParam = info.Length() > 9
                                  ? static_cast<uint64_t>(info[9].As<Napi::Number>().Int64Value())
                                  : 0;
      uint32_t n = static_cast<uint32_t>(startNs.ElementLength());
      FloxBacktestStats s{};
      int ok = flox_backtest_runner_run_bars(
          _handle,
          reinterpret_cast<const int64_t*>(startNs.Data()),
          reinterpret_cast<const int64_t*>(endNs.Data()),
          openA.Data(), highA.Data(), lowA.Data(), closeA.Data(), volA.Data(),
          n, symbol.c_str(), barType, barTypeParam, &s);
      if (!ok)
      {
        return info.Env().Null();
      }""",
        new="""      std::string symbol = info[7].As<Napi::String>().Utf8Value();
      uint8_t barType = info.Length() > 8 ? static_cast<uint8_t>(info[8].As<Napi::Number>().Uint32Value()) : 0;
      uint64_t barTypeParam = info.Length() > 9
                                  ? static_cast<uint64_t>(info[9].As<Napi::Number>().Int64Value())
                                  : 0;
      uint32_t n = static_cast<uint32_t>(startNs.ElementLength());
      FloxBacktestStats s{};
      int ok = flox_backtest_runner_run_bars(
          _handle,
          reinterpret_cast<const int64_t*>(startNs.Data()),
          reinterpret_cast<const int64_t*>(endNs.Data()),
          openA.Data(), highA.Data(), lowA.Data(), closeA.Data(), volA.Data(),
          n, symbol.c_str(), barType, barTypeParam, &s);
      if (!requireSameLength(info.Env(), "runBars",
                             {startNs.ElementLength(), endNs.ElementLength(),
                              openA.ElementLength(), highA.ElementLength(),
                              lowA.ElementLength(), closeA.ElementLength(),
                              volA.ElementLength()}))
      {
        return info.Env().Undefined();
      }
      if (!ok)
      {
        return info.Env().Null();
      }""",
        test="test_run_bars_array_lengths.js",
    ),

    # ── 9-12: one ts-taking method left on Int64Value, per class ───────────
    Mutation(
        name="live-queue-position-onorderplaced-int64value",
        why="live_queue_position.h OnOrderPlaced goes back to reading ts_ns through "
            "Napi::Number::Int64Value(), rejecting a BigInt and truncating a real "
            "wall-clock reading",
        file="live_queue_position.h",
        old="""    int64_t ts_ns = info.Length() > 6 ? toInt64Ns(info[6]) : 0;
    flox_live_queue_position_on_order_placed(""",
        new="""    int64_t ts_ns = info.Length() > 6 ? info[6].As<Napi::Number>().Int64Value() : 0;
    flox_live_queue_position_on_order_placed(""",
        test="test_ns_timestamp_round_trip.js",
    ),
    Mutation(
        name="feed-clock-tick-int64value",
        why="feed_clock.h MultiFeedClockWrap::Tick goes back to reading ts through "
            "Napi::Number::Int64Value()",
        file="feed_clock.h",
        old="""  Napi::Value Tick(const Napi::CallbackInfo& info)
  {
    int64_t ts = toInt64Ns(info[0]);""",
        new="""  Napi::Value Tick(const Napi::CallbackInfo& info)
  {
    int64_t ts = info[0].As<Napi::Number>().Int64Value();""",
        test="test_ns_timestamp_round_trip.js",
    ),
    Mutation(
        name="twap-starttimens-int64value",
        why="execution_algos.h TWAPWrap goes back to reading startTimeNs through "
            "Napi::Number::Int64Value(), rejecting a BigInt clock reading",
        file="execution_algos.h",
        old="""    // A clock reading, not a duration: read it the way every other ns
    // argument in the addon is read.
    int64_t start_time_ns = toInt64Ns(opts.Get("startTimeNs"));""",
        new="""    int64_t start_time_ns = opts.Get("startTimeNs").As<Napi::Number>().Int64Value();""",
        test="test_execution_algos.js",
    ),
    Mutation(
        name="signalbuilder-buy-number-only",
        why="engine.h SignalBuilderWrap::Buy goes back to Number-only for the ts "
            "argument, so signals.buy(engine.ts()[i], qty) -- a BigInt element out of "
            "Engine.ts() -- throws instead of round-tripping",
        file="engine.h",
        old="""  Napi::Value Buy(const Napi::CallbackInfo& info)
  {
    add(toInt64Ns(info[0]), 0, info[1].As<Napi::Number>().DoubleValue(), 0, 0, getSym(info, 2));""",
        new="""  Napi::Value Buy(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 0, info[1].As<Napi::Number>().DoubleValue(), 0, 0, getSym(info, 2));""",
        test="test_bindings.js",
    ),

    # ── 13: lastUpdateNs written back as a Number ───────────────────────────
    Mutation(
        name="live-queue-position-lastupdatens-number",
        why="live_queue_position.h Snapshot: lastUpdateNs goes back to "
            "Napi::Number::New(static_cast<double>(...)), quantising the estimator's "
            "stored clock reading",
        file="live_queue_position.h",
        old='''    out.Set("lastUpdateNs", Napi::BigInt::New(env, slots[3]));''',
        new='''    out.Set("lastUpdateNs", Napi::Number::New(env, static_cast<double>(slots[3])));''',
        test="test_ns_timestamp_round_trip.js",
    ),

    # ── 14: Engine.ts() back to Float64Array ────────────────────────────────
    Mutation(
        name="engine-ts-back-to-float64array",
        why="engine.h EngineWrap::Timestamps goes back to Float64Array, quantising a "
            "present-day ns reading to 256 ns steps and breaking SignalBuilder.buy's "
            "documented pairing with Engine.ts()",
        file="engine.h",
        old="""    auto& bars = resolve(info).bars;
    auto buf = Napi::BigInt64Array::New(info.Env(), bars.size());
    for (size_t i = 0; i < bars.size(); ++i)
    {
      buf[i] = bars[i].timestamp_ns;
    }""",
        new="""    auto& bars = resolve(info).bars;
    auto buf = Napi::Float64Array::New(info.Env(), bars.size());
    for (size_t i = 0; i < bars.size(); ++i)
    {
      buf[i] = static_cast<double>(bars[i].timestamp_ns);
    }""",
        test="test_bindings.js",
    ),

    # ── 15-18: callGate policy (finding 22) ─────────────────────────────────
    Mutation(
        name="callgate-napi-error-escapes",
        why="hooks.h callGate: the try/catch around fn.Call() is removed, so a throwing "
            "JS gate unwinds out through the C function pointer the engine called it by "
            "and resurfaces at emit.marketBuy() -- the exact bug the policy exists to "
            "close",
        file="hooks.h",
        old="""  Napi::Value result;
  try
  {
    result = fn.Call({signalToJs(env, sig)});
  }
  catch (const Napi::Error& e)
  {
    // node-addon-api cleared the pending JS exception when it built this.
    reportHookError(sink, hook, method, e.Message());
    return 0;
  }
  catch (const std::exception& e)
  {
    reportHookError(sink, hook, method, e.what());
    return 0;
  }
  if (!result.IsBoolean())""",
        new="""  Napi::Value result = fn.Call({signalToJs(env, sig)});
  if (!result.IsBoolean())""",
        test="test_gate_failure_policy.js",
    ),
    Mutation(
        name="callgate-nonboolean-treated-as-true",
        why="hooks.h callGate: a non-boolean return is treated as allow (returns 1) "
            "instead of deny, and is not reported",
        file="hooks.h",
        old="""  if (!result.IsBoolean())
  {
    reportHookError(sink, hook, method,
                    std::string("returned a non-boolean (") + result.ToString().Utf8Value() +
                        "); a gate must return a boolean");
    return 0;
  }
  return result.As<Napi::Boolean>().Value() ? 1u : 0u;""",
        new="""  if (!result.IsBoolean())
  {
    return 1;
  }
  return result.As<Napi::Boolean>().Value() ? 1u : 0u;""",
        test="test_gate_failure_policy.js",
    ),
    Mutation(
        name="callgate-throw-not-recorded",
        why="hooks.h callGate: the Napi::Error catch stops calling reportHookError, so "
            "runner.hookErrors() has no record of a throwing gate even though the order "
            "is still (correctly) denied",
        file="hooks.h",
        old="""  catch (const Napi::Error& e)
  {
    // node-addon-api cleared the pending JS exception when it built this.
    reportHookError(sink, hook, method, e.Message());
    return 0;
  }""",
        new="""  catch (const Napi::Error& e)
  {
    return 0;
  }""",
        test="test_gate_failure_policy.js",
    ),
    Mutation(
        name="killswitch-records-wrong-hook-name",
        why="hooks.h KillSwitchHost::checkBridge: passes \"riskManager\" to callGate "
            "instead of \"killSwitch\", so a KillSwitch.check failure is misattributed "
            "in hookErrors()",
        file="hooks.h",
        old="""    auto* self = static_cast<KillSwitchHost*>(ud);
    return callGate(self->check_fn, self->env, sig, self->errors, "killSwitch", "check");""",
        new="""    auto* self = static_cast<KillSwitchHost*>(ud);
    return callGate(self->check_fn, self->env, sig, self->errors, "riskManager", "check");""",
        test="test_gate_failure_policy.js",
    ),

    # ── 19: Executor.capabilities() guard removed ───────────────────────────
    Mutation(
        name="capabilities-unguarded",
        why="hooks.h ExecutorHost::capabilitiesBridge: the try/catch around "
            "capabilities_fn.Call({}) is removed, so a throwing capabilities() would "
            "unwind out through the C function pointer the engine calls it by (same "
            "boundary as the gates). No node/test file makes capabilities() throw, so "
            "this is expected to survive",
        file="hooks.h",
        old="""    // Same boundary as the gates: this is called through a C function
    // pointer, so a throw out of it is reported rather than unwound. An
    // executor that cannot say what it supports supports nothing.
    Napi::Value result;
    try
    {
      result = self->capabilities_fn.Call({});
    }
    catch (const Napi::Error& e)
    {
      reportHookError(self->errors, "executor", "capabilities", e.Message());
      return;
    }
    catch (const std::exception& e)
    {
      reportHookError(self->errors, "executor", "capabilities", e.what());
      return;
    }
    if (!result.IsObject())""",
        new="""    Napi::Value result = self->capabilities_fn.Call({});
    if (!result.IsObject())""",
        test="test_gate_failure_policy.js",
    ),

    # ── 20-26: one setter not refusing on a threaded Runner, each of seven
    # (finding 24) ───────────────────────────────────────────────────────────
    Mutation(
        name="setpnltracker-not-guarded",
        why="strategy.h setPnlTracker: the requireSyncHook guard is removed, so a "
            "threaded Runner accepts a PnLTracker and wires it into the LiveEngine, "
            "where a C++ consumer thread would call it. test_single_hook_mode.js's "
            "INLINE_HOOKS list only covers setRiskManager/setKillSwitch/"
            "setOrderValidator/setExecutor -- setPnlTracker is not in it, so this is "
            "expected to survive",
        file="strategy.h",
        old="""    if (!requireSyncHook(env, "setPnlTracker"))
    {
      return env.Undefined();
    }
    _pnl_host = std::make_unique<flox_node::PnLTrackerHost>(env, info[0].As<Napi::Object>());""",
        new="""    _pnl_host = std::make_unique<flox_node::PnLTrackerHost>(env, info[0].As<Napi::Object>());""",
        test="test_single_hook_mode.js",
    ),
    Mutation(
        name="setstoragesink-not-guarded",
        why="strategy.h setStorageSink: the requireSyncHook guard is removed. "
            "setStorageSink is likewise absent from test_single_hook_mode.js's "
            "INLINE_HOOKS list, so this is expected to survive",
        file="strategy.h",
        old="""    if (!requireSyncHook(env, "setStorageSink"))
    {
      return env.Undefined();
    }
    _storage_host = std::make_unique<flox_node::StorageSinkHost>(env, info[0].As<Napi::Object>());""",
        new="""    _storage_host = std::make_unique<flox_node::StorageSinkHost>(env, info[0].As<Napi::Object>());""",
        test="test_single_hook_mode.js",
    ),
    Mutation(
        name="setriskmanager-not-guarded",
        why="strategy.h setRiskManager: the requireSyncHook guard is removed -- this one "
            "is in test_single_hook_mode.js's INLINE_HOOKS list",
        file="strategy.h",
        old="""    if (!requireSyncHook(env, "setRiskManager"))
    {
      return env.Undefined();
    }
    _risk_host = std::make_unique<flox_node::RiskManagerHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        new="""    _risk_host = std::make_unique<flox_node::RiskManagerHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        test="test_single_hook_mode.js",
    ),
    Mutation(
        name="setkillswitch-not-guarded",
        why="strategy.h setKillSwitch: the requireSyncHook guard is removed -- this one "
            "is in test_single_hook_mode.js's INLINE_HOOKS list",
        file="strategy.h",
        old="""    if (!requireSyncHook(env, "setKillSwitch"))
    {
      return env.Undefined();
    }
    _kill_host = std::make_unique<flox_node::KillSwitchHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        new="""    _kill_host = std::make_unique<flox_node::KillSwitchHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        test="test_single_hook_mode.js",
    ),
    Mutation(
        name="setordervalidator-not-guarded",
        why="strategy.h setOrderValidator: the requireSyncHook guard is removed -- this "
            "one is in test_single_hook_mode.js's INLINE_HOOKS list",
        file="strategy.h",
        old="""    if (!requireSyncHook(env, "setOrderValidator"))
    {
      return env.Undefined();
    }
    _validator_host =
        std::make_unique<flox_node::OrderValidatorHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        new="""    _validator_host =
        std::make_unique<flox_node::OrderValidatorHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        test="test_single_hook_mode.js",
    ),
    Mutation(
        name="setmarketdatarecorder-not-guarded",
        why="strategy.h setMarketDataRecorder: the requireSyncHook guard is removed. "
            "setMarketDataRecorder is absent from test_single_hook_mode.js's "
            "INLINE_HOOKS list, so this is expected to survive",
        file="strategy.h",
        old="""    if (!requireSyncHook(env, "setMarketDataRecorder"))
    {
      return env.Undefined();
    }

    auto obj = info[0].As<Napi::Object>();""",
        new="""    auto obj = info[0].As<Napi::Object>();""",
        test="test_single_hook_mode.js",
    ),
    Mutation(
        name="setexecutor-not-guarded",
        why="strategy.h setExecutor: the requireSyncHook guard is removed -- this one is "
            "in test_single_hook_mode.js's INLINE_HOOKS list",
        file="strategy.h",
        old="""    if (!requireSyncHook(env, "setExecutor"))
    {
      return env.Undefined();
    }
    _executor_host =
        std::make_unique<flox_node::ExecutorHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        new="""    _executor_host =
        std::make_unique<flox_node::ExecutorHost>(env, info[0].As<Napi::Object>(), &_hook_errors);""",
        test="test_single_hook_mode.js",
    ),

    # ── 27: setX(null) refused too ──────────────────────────────────────────
    Mutation(
        name="setriskmanager-null-also-refused",
        why="strategy.h setRiskManager: requireSyncHook is moved ahead of the null/"
            "undefined detach check, so runner.setRiskManager(null) on a threaded "
            "Runner throws instead of being accepted as a detach",
        file="strategy.h",
        old="""  Napi::Value setRiskManager(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _risk_host.reset();
      if (_mode == Mode::Sync)
      {
        flox_runner_set_risk_manager(_runner, nullptr);
      }
      else
      {
        flox_live_engine_set_risk_manager(_engine, nullptr);
      }
      return env.Undefined();
    }
    if (!requireSyncHook(env, "setRiskManager"))
    {
      return env.Undefined();
    }""",
        new="""  Napi::Value setRiskManager(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (!requireSyncHook(env, "setRiskManager"))
    {
      return env.Undefined();
    }
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      _risk_host.reset();
      if (_mode == Mode::Sync)
      {
        flox_runner_set_risk_manager(_runner, nullptr);
      }
      else
      {
        flox_live_engine_set_risk_manager(_engine, nullptr);
      }
      return env.Undefined();
    }""",
        test="test_single_hook_mode.js",
    ),

    # ── 28: one of the four newly-delivered events swallowed (finding 24,
    # side effect) -- our own, adversarial ─────────────────────────────────
    Mutation(
        name="onpartiallyfilled-event-swallowed",
        why="hooks.h ExecutionListenerHost::onPartialBridge: the fix's own note says "
            "onPartiallyFilled/onRejected/onReplaced/onTrailingStopUpdated 'used to go "
            "unconditionally to post() at a closed channel, so were never called -- now "
            "they are.' This reintroduces exactly that: the bridge returns without "
            "calling on_partial_fn, so a real partial fill never reaches JS. No "
            "node/test file references onPartiallyFilled, onRejected, onReplaced or "
            "onTrailingStopUpdated at all, so this is expected to survive",
        file="hooks.h",
        old="""  static void onPartialBridge(void* ud, const FloxOrder* o, int64_t fill_qty)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_partial_fn.IsEmpty())
    {
      return;
    }
    self->on_partial_fn.Call({""",
        new="""  static void onPartialBridge(void* ud, const FloxOrder* o, int64_t fill_qty)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    return;
    if (self->on_partial_fn.IsEmpty())
    {
      return;
    }
    self->on_partial_fn.Call({""",
        test=None,  # whole-suite only: no test file names this handler
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


def addon_stat() -> tuple[int, float] | None:
    if not ADDON.exists():
        return None
    st = ADDON.stat()
    return (st.st_size, st.st_mtime)


def rebuild() -> str:
    if OBJECT.exists():
        OBJECT.unlink()
    before = addon_stat()
    result = subprocess.run(
        [NPM, "run", "build"],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=NODE_DIR,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"rebuild failed:\n{output[-4000:]}")
    if "Building CXX object" not in output or "flox_node.cpp.o" not in output:
        raise SystemExit(
            "rebuild compiled nothing for flox_node.cpp.o -- the result would have been "
            f"a stale addon, so the run is refused:\n{output[-2000:]}"
        )
    after = addon_stat()
    if after is None:
        raise SystemExit(f"rebuild reported success but {ADDON} does not exist")
    if before is not None and before == after:
        raise SystemExit(
            f"rebuild reported success but {ADDON} has the same size and mtime as before "
            "-- it was not actually relinked"
        )
    print(f"  addon    before {before}  after {after}")
    return output


def run_test(path: Path, timeout: int) -> tuple[int, str]:
    try:
        result = subprocess.run(
            ["node", str(path)], capture_output=True, text=True, timeout=timeout, cwd=NODE_DIR,
        )
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout}s"
    return result.returncode, result.stdout + result.stderr


def all_suite_tests() -> list[Path]:
    return sorted(TEST_DIR.glob("test_*.js"))


def control() -> bool:
    print("control build")
    rebuild()
    ok = True
    tests = sorted({m.test for m in MUTATIONS if m.test} | {"test_array_length_safety.js"})
    for name in tests:
        code, output = run_test(TEST_DIR / name, TEST_TIMEOUT)
        state = "green" if code == 0 else "RED"
        summary = next((line for line in output.splitlines() if "passed" in line and "failed" in line), "")
        print(f"  control {name:<40} {state}   {summary.strip()}")
        ok = ok and code == 0
    return ok


def sweep() -> tuple[Path | None, int, str]:
    """Run every node/test/test_*.js file. Returns the first one that goes
    red, or (None, 0, '') if the whole suite stayed green."""
    for path in all_suite_tests():
        code, output = run_test(path, SWEEP_TIMEOUT)
        if code != 0:
            return path, code, output
    return None, 0, ""


def run_mutation(m: Mutation) -> tuple[bool, str]:
    """Returns (killed, killer_description)."""
    path = SRC_DIR / m.file
    original = path.read_text()
    before = sha256(path)
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    node/src/{m.file}")
    print(f"  sha256  before  {before}")

    mutated = replace_occurrence(original, m.old, m.new, m.occurrence, m.expected_occurrences)
    if mutated == original:
        raise SystemExit("mutation changed nothing")
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")

    killed = False
    killer = ""
    try:
        rebuild()

        if m.test:
            code, output = run_test(TEST_DIR / m.test, TEST_TIMEOUT)
            print(f"  node test/{m.test} -> exit {code} "
                  f"({'RED, mutation killed' if code != 0 else 'green'})")
            if code != 0:
                killed = True
                killer = m.test
            elif code == 0:
                print("  ----- primary test output (tail) -----")
                print("\n".join(output.splitlines()[-10:]))

        if not killed:
            print("  primary test did not catch it (or none named) -- sweeping the whole suite")
            red_path, code, output = sweep()
            if red_path is not None:
                killed = True
                killer = red_path.name
                print(f"  whole-suite sweep: {red_path.name} -> exit {code} (RED, mutation killed)")
                print("  ----- sweep output (tail) -----")
                print("\n".join(output.splitlines()[-10:]))
            else:
                print("  whole-suite sweep: every test_*.js stayed GREEN -- MUTATION SURVIVED")
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit("restore failed: the file does not hash back to its original")
        if OBJECT.exists():
            OBJECT.unlink()

    return killed, killer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            tag = "equivalent" if m.equivalent else ""
            print(f"{m.name:<44} {m.test or '(whole suite only)':<32} {tag}")
        return 0

    if not (BUILD_DIR / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD_DIR} is not configured; run\n"
            "  cd node && npm install && npm run build"
        )

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    print("=" * 70)
    print("control run before the mutations")
    print("=" * 70)
    if not control():
        raise SystemExit("the unmutated addon is not green; nothing below would mean anything")

    t0 = time.time()
    results = [(m, *run_mutation(m)) for m in selected]
    elapsed = time.time() - t0

    print("\n" + "=" * 70)
    print("control run after the mutations")
    print("=" * 70)
    restored = control()

    print("\n" + "=" * 70)
    print(f"summary ({elapsed:.0f}s)")
    print("=" * 70)
    for m, killed, killer in results:
        tag = "equivalent" if m.equivalent else ""
        status = "RED  " if killed else "ALIVE"
        by = f"  (caught by {killer})" if killed else ""
        print(f"  {status}  {m.name:<44} {tag}{by}")

    survived = [(m, killer) for m, killed, killer in results if not killed and not m.equivalent]
    if survived:
        print(f"\n{len(survived)} unexplained survivor(s):")
        for m, _ in survived:
            print(f"  - {m.name}: {m.why}")
    if not restored:
        print("\nthe tree did not come back green after the run")

    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
