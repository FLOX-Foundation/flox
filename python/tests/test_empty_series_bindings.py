"""python/tests/test_empty_series_bindings.py

Regression tests for one defect class across the pybind11 binding layer:
every function that hands a numpy array back to Python fills it from a
contiguous C++ container with a bare ``memcpy``, and a container with no
elements owns no buffer at all -- ``std::vector::data()`` returns a null
pointer before the first allocation. Passing that to ``memcpy`` is
undefined even when the length is zero, and glibc says so out loud by
declaring ``memcpy`` nonnull, so a build with the undefined-behaviour
sanitizer turned on stops the process at the call while the release build
quietly returns the empty array the caller expected.

An empty series is not a corner nobody reaches. An indicator over a feed
that has not produced a bar yet, a graph field read after ``reset()``
cleared the bars, a window that matched no rows -- all three arrive here
through the documented API on the first call.

Each case below asserts the answer the release build gives (an empty
float64 array, or NaN), so the file is worth running on its own, and so a
sanitizer build reports at the copy instead of coming back green.

Run from repo root:
    PYTHONPATH=build/python python3 -m pytest python/tests/test_empty_series_bindings.py
"""
from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import lrvx  # noqa: E402


def empty() -> np.ndarray:
    return np.array([], dtype=np.float64)


class EmptySeriesMixin(unittest.TestCase):
    def assertEmptyDoubleArray(self, arr, what: str) -> None:
        self.assertIsInstance(arr, np.ndarray, what)
        self.assertEqual(arr.dtype, np.dtype(np.float64), what)
        self.assertEqual(arr.size, 0, what)


class SingleInputIndicatorTests(EmptySeriesMixin):
    """One array in, one array out -- the largest group of copies."""

    CASES = [
        ("SMA", lambda: lrvx.SMA(5)),
        ("EMA", lambda: lrvx.EMA(5)),
        ("RMA", lambda: lrvx.RMA(5)),
        ("RSI", lambda: lrvx.RSI(14)),
        ("DEMA", lambda: lrvx.DEMA(5)),
        ("TEMA", lambda: lrvx.TEMA(5)),
        ("KAMA", lambda: lrvx.KAMA(10)),
        ("Slope", lambda: lrvx.Slope(3)),
        ("Skewness", lambda: lrvx.Skewness(5)),
        ("Kurtosis", lambda: lrvx.Kurtosis(5)),
        ("RollingZScore", lambda: lrvx.RollingZScore(5)),
        ("ShannonEntropy", lambda: lrvx.ShannonEntropy(5)),
        ("AutoCorrelation", lambda: lrvx.AutoCorrelation(5, 1)),
    ]

    def test_compute_on_an_empty_series(self) -> None:
        for name, ctor in self.CASES:
            with self.subTest(indicator=name):
                self.assertEmptyDoubleArray(ctor().compute(empty()), f"{name}.compute([])")


class MultiInputIndicatorTests(EmptySeriesMixin):
    """Bar, high/low, OHLC and pair inputs, plus the dict-returning ones."""

    def test_bar_and_pair_indicators(self) -> None:
        e = empty()
        self.assertEmptyDoubleArray(lrvx.ATR(14).compute(e, e, e), "ATR")
        self.assertEmptyDoubleArray(lrvx.CCI(20).compute(e, e, e), "CCI")
        self.assertEmptyDoubleArray(lrvx.ParkinsonVol(5).compute(e, e), "ParkinsonVol")
        self.assertEmptyDoubleArray(
            lrvx.RogersSatchellVol(5).compute(e, e, e, e), "RogersSatchellVol"
        )
        self.assertEmptyDoubleArray(lrvx.Correlation(5).compute(e, e), "Correlation")

    def test_indicators_returning_several_arrays(self) -> None:
        e = empty()

        macd = lrvx.MACD().compute(e)
        for key in ("line", "signal", "histogram"):
            self.assertEmptyDoubleArray(macd[key], f"MACD.compute([])[{key!r}]")

        boll = lrvx.Bollinger(20, 2.0).compute(e)
        for key in ("upper", "middle", "lower"):
            self.assertEmptyDoubleArray(boll[key], f"Bollinger.compute([])[{key!r}]")

        stoch = lrvx.Stochastic(14, 3).compute(e, e, e)
        for key in ("k", "d"):
            self.assertEmptyDoubleArray(stoch[key], f"Stochastic.compute([])[{key!r}]")


class ModuleLevelIndicatorTests(EmptySeriesMixin):
    """The free functions, which copy through a separately written path."""

    def test_single_array_results(self) -> None:
        e = empty()
        self.assertEmptyDoubleArray(lrvx.chop(e, e, e, 14), "chop")
        self.assertEmptyDoubleArray(lrvx.dema(e, 5), "dema")
        self.assertEmptyDoubleArray(lrvx.tema(e, 5), "tema")
        self.assertEmptyDoubleArray(lrvx.cci(e, e, e, 20), "cci")
        self.assertEmptyDoubleArray(lrvx.vwap(e, e), "vwap")
        self.assertEmptyDoubleArray(lrvx.cvd(e, e, e, e, e), "cvd")
        empty_flags = np.array([], dtype=np.int8)
        self.assertEmptyDoubleArray(
            lrvx.trade_pnl(empty_flags, empty_flags, e), "trade_pnl"
        )

    def test_dict_results(self) -> None:
        e = empty()

        adx = lrvx.adx(e, e, e, 14)
        for key in ("adx", "plus_di", "minus_di"):
            self.assertEmptyDoubleArray(adx[key], f"adx(...)[{key!r}]")

        boll = lrvx.bollinger(e, 20, 2.0)
        for key in ("upper", "middle", "lower"):
            self.assertEmptyDoubleArray(boll[key], f"bollinger(...)[{key!r}]")

        stoch = lrvx.stochastic(e, e, e, 14, 3)
        for key in ("k", "d"):
            self.assertEmptyDoubleArray(stoch[key], f"stochastic(...)[{key!r}]")


class IndicatorGraphEmptyTests(EmptySeriesMixin):
    """The graph, where an empty series arrives without the caller asking."""

    def test_fields_on_a_symbol_with_no_bars(self) -> None:
        g = lrvx.IndicatorGraph()
        for field in ("close", "high", "low", "volume"):
            with self.subTest(field=field):
                self.assertEmptyDoubleArray(getattr(g, field)(0), f"graph.{field}(0)")

    def test_node_computed_over_no_bars(self) -> None:
        g = lrvx.IndicatorGraph()
        g.add_node("double_close", [], lambda graph, sym: graph.close(sym) * 2.0)
        self.assertEmptyDoubleArray(g.require(0, "double_close"), "require over no bars")
        self.assertTrue(math.isnan(g.current(0, "double_close")))

    def test_fields_after_reset(self) -> None:
        g = lrvx.IndicatorGraph()
        g.add_node("double_close", [], lambda graph, sym: graph.close(sym) * 2.0)
        for close in (10.0, 20.0, 30.0):
            g.step(0, close)
        self.assertEqual(g.bar_count(0), 3)
        self.assertFalse(math.isnan(g.current(0, "double_close")))

        g.reset(0)
        self.assertEqual(g.bar_count(0), 0)
        self.assertEmptyDoubleArray(g.close(0), "graph.close(0) after reset")
        self.assertTrue(math.isnan(g.current(0, "double_close")))


if __name__ == "__main__":
    unittest.main()
