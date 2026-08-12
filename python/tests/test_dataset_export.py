"""python/tests/test_dataset_export.py — point-in-time dataset export contract.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_dataset_export.py

Two contracts under test:
  1. Point-in-time: row t of the dataset depends only on bars 0..t —
     recomputing on a truncated input yields identical rows (no lookahead).
  2. Online/offline parity: the batch column equals the streaming update()
     path value for value, because it IS the same code.
"""

import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'python')
sys.path.insert(0, os.path.abspath(build_dir))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

import numpy as np

import flox_py as flox
from flox_py.dataset import build_dataset, _register_feature

try:  # pragma: no cover - environment guard
    import pytest
except ImportError:  # running as a plain script
    pytest = None

# The dataset builder needs the indicator-graph binding. Skip (do not error)
# when the built module predates it, so a stale local build stays green while
# a fresh CI build runs the real assertions below.
from flox_py import _flox_py as _core
_HAS_GRAPH = hasattr(_core, "IndicatorGraph")
if pytest is not None:
    pytestmark = pytest.mark.skipif(
        not _HAS_GRAPH, reason="built flox_py has no IndicatorGraph (rebuild the binding)")

_passed = 0
_failed = 0


def check(condition, msg):
    global _passed, _failed
    if condition:
        print(f"  ok  {msg}")
        _passed += 1
    else:
        print(f"  FAIL  {msg}")
        _failed += 1
    # A real assertion so `pytest` actually fails on regression -- previously
    # this only counted and the counter was inspected only under __main__, so
    # the point-in-time / no-lookahead tests always passed under the CI runner.
    assert condition, msg


def synthetic_bars(n=300, seed=7):
    rng = np.random.default_rng(seed)
    close = 100.0 + np.cumsum(rng.normal(0, 0.5, n))
    high = close + rng.uniform(0.0, 0.4, n)
    low = close - rng.uniform(0.0, 0.4, n)
    volume = rng.uniform(1.0, 10.0, n)
    ts = np.arange(n, dtype=np.int64) * 60_000_000_000
    return {"ts_ns": ts, "close": close, "high": high, "low": low, "volume": volume}


def test_basic_shape():
    bars = synthetic_bars()
    ds = build_dataset(bars=bars, features=["sma_20", "rsi_14"])
    check(ds["X"].shape[1] == 2, "two feature columns")
    check(len(ds["ts_ns"]) == len(ds["X"]), "timestamps aligned with X")
    check(not np.isnan(ds["X"]).any(), "warmup rows dropped")
    check(ds["feature_names"] == ["sma_20", "rsi_14"], "feature names preserved")


def test_point_in_time_no_lookahead():
    bars = synthetic_bars()
    full = build_dataset(bars=bars, features=["sma_20"], drop_warmup=False)

    cut = 200
    truncated_bars = {k: v[:cut] for k, v in bars.items()}
    trunc = build_dataset(bars=truncated_bars, features=["sma_20"], drop_warmup=False)

    same = np.allclose(
        full["X"][:cut], trunc["X"], equal_nan=True
    )
    check(same, "rows 0..cut identical on truncated input (no lookahead)")


def test_online_offline_parity():
    bars = synthetic_bars()
    ds = build_dataset(bars=bars, features=["sma_20"], drop_warmup=False)

    # Stream the same bars through the same graph type, step by step.
    g = flox.IndicatorGraph()
    _register_feature(g, 0, "sma_20")
    streamed = []
    for i in range(len(bars["close"])):
        g.step(0, bars["close"][i], bars["high"][i], bars["low"][i], bars["volume"][i])
        streamed.append(g.current(0, "sma_20"))
    streamed = np.array(streamed)

    same = np.allclose(ds["X"][:, 0], streamed, equal_nan=True)
    check(same, "batch dataset column == streaming path value for value")


def test_labels_are_forward_returns():
    bars = synthetic_bars()
    h = 5
    ds = build_dataset(
        bars=bars, features=["sma_20"], label_horizon_bars=h, drop_warmup=False
    )
    close = bars["close"]
    expected = np.log(close[h:] / close[:-h])
    check(len(ds["y"]) == len(ds["X"]), "labels aligned with X")
    check(np.allclose(ds["y"], expected), "y[t] = log(close[t+h]/close[t])")
    check(len(ds["X"]) == len(close) - h, "last h rows dropped")


def test_input_validation():
    bars = synthetic_bars(50)
    try:
        build_dataset(bars=bars, features=[])
        check(False, "empty features rejected")
    except ValueError:
        check(True, "empty features rejected")
    try:
        build_dataset(features=["sma_20"])
        check(False, "missing source rejected")
    except ValueError:
        check(True, "missing source rejected")
    try:
        build_dataset(bars=bars, features=["sma_20"], label_horizon_bars=100)
        check(False, "oversized horizon rejected")
    except ValueError:
        check(True, "oversized horizon rejected")


if __name__ == "__main__":
    test_basic_shape()
    test_point_in_time_no_lookahead()
    test_online_offline_parity()
    test_labels_are_forward_returns()
    test_input_validation()
    print(f"\n{_passed} passed, {_failed} failed")
    sys.exit(1 if _failed else 0)
