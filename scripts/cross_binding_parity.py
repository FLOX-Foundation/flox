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

    print(f"\n{ok} passed, {bad} failed")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
