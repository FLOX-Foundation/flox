#!/usr/bin/env python3
"""
scripts/cross_binding_parity.py

Cross-binding parity check. Runs the same indicators on the same inputs
through Python and Node, and asserts the outputs agree to 1e-9.

CI gate: blocking. Drift between bindings on identical math is a bug — the
unified-source-of-truth refactor should make these tautologies, but the
test is here to catch regressions if anyone re-introduces parallel
implementations.

Run from repo root:
    PYTHONPATH=build/python python3 scripts/cross_binding_parity.py
"""

from __future__ import annotations

import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
NODE_DIR = REPO / "node"
NODE_BIN = "node"


def run_node(script: str) -> dict:
    """Run a JS snippet that prints one JSON line; return parsed dict."""
    res = subprocess.run(
        [NODE_BIN, "-e", script],
        capture_output=True,
        text=True,
        cwd=str(NODE_DIR),
        timeout=60,
    )
    if res.returncode != 0:
        raise RuntimeError(f"node failed: {res.stderr}")
    last = res.stdout.strip().splitlines()[-1]
    return json.loads(last)


def main() -> int:
    sys.path.insert(0, str(REPO / "build" / "python"))
    try:
        import flox_py as flox  # noqa
    except ImportError as e:
        print(f"error: cannot import flox_py: {e}", file=sys.stderr)
        return 1

    rng = np.random.default_rng(42)
    prices = rng.uniform(100.0, 200.0, 50).tolist()
    spread = rng.uniform(0.5, 5.0, 50)
    high = (np.array(prices) + spread).tolist()
    low = (np.array(prices) - spread).tolist()
    close = prices

    # Cases: (indicator_name, py_call, js_call_template, atol)
    # JS template uses prices/high/low/close from a JS-side fixture below.
    js_setup = (
        f"const flox = require('.');\n"
        f"const prices = {prices};\n"
        f"const high = {high};\n"
        f"const low = {low};\n"
        f"const close = {close};\n"
    )

    cases = [
        ("EMA(10)",
         lambda: list(flox.EMA(10).compute(np.array(prices))),
         "const out = Array.from(new flox.EMA(10).compute(new Float64Array(prices)));"),
        ("SMA(10)",
         lambda: list(flox.SMA(10).compute(np.array(prices))),
         "const out = Array.from(new flox.SMA(10).compute(new Float64Array(prices)));"),
        ("RSI(14)",
         lambda: list(flox.RSI(14).compute(np.array(prices))),
         "const out = Array.from(new flox.RSI(14).compute(new Float64Array(prices)));"),
        ("ATR(14)",
         lambda: list(flox.ATR(14).compute(np.array(high), np.array(low), np.array(close))),
         "const out = Array.from(new flox.ATR(14).compute("
         "new Float64Array(high), new Float64Array(low), new Float64Array(close)));"),
        ("CCI(20)",
         lambda: list(flox.CCI(20).compute(np.array(high), np.array(low), np.array(close))),
         "const out = Array.from(new flox.CCI(20).compute("
         "new Float64Array(high), new Float64Array(low), new Float64Array(close)));"),
        ("DEMA(10)",
         lambda: list(flox.DEMA(10).compute(np.array(prices))),
         "const out = Array.from(new flox.DEMA(10).compute(new Float64Array(prices)));"),
        ("Skewness(10)",
         lambda: list(flox.Skewness(10).compute(np.array(prices))),
         "const out = Array.from(new flox.Skewness(10).compute(new Float64Array(prices)));"),
    ]

    ok = 0
    bad = 0
    for name, py_fn, js_call in cases:
        py_out = py_fn()
        js_out = run_node(
            js_setup
            + js_call
            + "\nprocess.stdout.write(JSON.stringify(out));"
        )
        # Replace NaN-like values consistently
        py_arr = np.array([v if (v is not None and math.isfinite(v)) else math.nan for v in py_out])
        js_arr = np.array([v if (v is not None and math.isfinite(v)) else math.nan for v in js_out])
        if py_arr.shape != js_arr.shape:
            print(f"  FAIL  {name}: shape mismatch py={py_arr.shape} js={js_arr.shape}")
            bad += 1
            continue
        nan_py = np.isnan(py_arr)
        nan_js = np.isnan(js_arr)
        if not np.array_equal(nan_py, nan_js):
            print(f"  FAIL  {name}: NaN mask mismatch")
            bad += 1
            continue
        valid = ~nan_py
        if valid.any():
            close_ok = np.allclose(py_arr[valid], js_arr[valid], atol=1e-9, rtol=1e-9)
            if not close_ok:
                worst = float(np.max(np.abs(py_arr[valid] - js_arr[valid])))
                print(f"  FAIL  {name}: max abs diff = {worst:.3e}")
                bad += 1
                continue
        print(f"  ok    {name}: parity within 1e-9")
        ok += 1

    # ── on_bar callback parity: Python and Node strategies must observe
    #    identical OHLC values when fed the same bars.
    print("\n  on_bar callback parity (Python vs Node):")
    bars_n = 8
    bar_open  = [100.0 + i * 1.5 for i in range(bars_n)]
    bar_high  = [o + 0.7 for o in bar_open]
    bar_low   = [o - 0.7 for o in bar_open]
    bar_close = [o + 0.3 for o in bar_open]
    bar_vol   = [10.0 for _ in range(bars_n)]
    bar_start = [i * 1_000_000_000 for i in range(bars_n)]
    bar_end   = [(i + 1) * 1_000_000_000 for i in range(bars_n)]

    # Python: subclass flox.Strategy, capture each bar
    reg_py = flox.SymbolRegistry()
    sym_py = reg_py.add_symbol("test", "TST", tick_size=0.01)
    captured_py = []

    class _BarCapture(flox.Strategy):
        def on_bar(self, ctx, bar):
            captured_py.append((bar.open, bar.high, bar.low, bar.close))

    strat_py = _BarCapture([sym_py])
    bt_py = flox.BacktestRunner(reg_py, fee_rate=0.0, initial_capital=10_000.0)
    bt_py.set_strategy(strat_py)
    bt_py.run_bars(
        np.array(bar_start, dtype=np.int64),
        np.array(bar_end,   dtype=np.int64),
        np.array(bar_open,  dtype=np.float64),
        np.array(bar_high,  dtype=np.float64),
        np.array(bar_low,   dtype=np.float64),
        np.array(bar_close, dtype=np.float64),
        np.array(bar_vol,   dtype=np.float64),
        "TST",
    )

    # Node: same flow, capture from JS strategy.onBar
    starts_js = [i * 1_000_000_000 for i in range(bars_n)]
    ends_js   = [(i + 1) * 1_000_000_000 for i in range(bars_n)]
    js_bars = (
        "const flox = require('.');\n"
        "const reg = new flox.SymbolRegistry();\n"
        "const sym = reg.addSymbol('test', 'TST', 0.01);\n"
        f"const startNs = new BigInt64Array({starts_js}.map(BigInt));\n"
        f"const endNs   = new BigInt64Array({ends_js}.map(BigInt));\n"
        f"const op = new Float64Array({bar_open});\n"
        f"const hi = new Float64Array({bar_high});\n"
        f"const lo = new Float64Array({bar_low});\n"
        f"const cl = new Float64Array({bar_close});\n"
        f"const vo = new Float64Array({bar_vol});\n"
        "const captured = [];\n"
        "const strat = { symbols: [sym], "
        "  onBar: (ctx, bar, em) => captured.push([bar.open, bar.high, bar.low, bar.close]) };\n"
        "const bt = new flox.BacktestRunner(reg, 0.0, 10000.0);\n"
        "bt.setStrategy(strat);\n"
        "bt.runBars(startNs, endNs, op, hi, lo, cl, vo, 'TST');\n"
        "process.stdout.write(JSON.stringify(captured));\n"
    )
    js_out = run_node(js_bars)

    py_arr = np.array(captured_py)
    js_arr = np.array(js_out)
    bar_pass = (py_arr.shape == js_arr.shape) and np.allclose(py_arr, js_arr, atol=1e-9)
    if bar_pass:
        print(f"  ok    on_bar: {len(captured_py)} bars, OHLC parity within 1e-9")
        ok += 1
    else:
        print(f"  FAIL  on_bar: py shape={py_arr.shape} js shape={js_arr.shape}")
        if py_arr.shape == js_arr.shape:
            worst = float(np.max(np.abs(py_arr - js_arr)))
            print(f"          max abs diff = {worst:.3e}")
        bad += 1

    print(f"\n{ok} passed, {bad} failed")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
