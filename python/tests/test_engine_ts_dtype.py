"""python/tests/test_engine_ts_dtype.py

``Engine.ts()`` returns nanosecond timestamps. It built them into a
``float64`` array, so every value above 2^53 came back rounded -- a real
2026 nanosecond timestamp is ~1.78e18, where the float64 spacing is 256 ns.
The sibling accessor on the event-driven path,
``BacktestRunner.equity_curve()``, already returns ``int64``; ``Engine.ts()``
is the odd one out, and nothing about a timestamp wants a float.

The OHLCV prices stay ``float64`` on purpose: they are converted out of
fixed point for the caller, and that is the documented shape of
``open()`` / ``high()`` / ``low()`` / ``close()`` / ``volume()``.

Run from repo root:
    PYTHONPATH=build/python python3 -m pytest python/tests/test_engine_ts_dtype.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import flox_py as flox  # noqa: E402

# Nanoseconds since epoch, with non-zero digits below the microsecond and
# far above 2^53 (9007199254740992). At this magnitude the float64 spacing
# is 256 ns, so a round trip through a double cannot return these.
TS_NS = [1_776_606_960_123_456_789, 1_776_607_020_123_456_789]


def _engine_with_bars() -> "flox.Engine":
    engine = flox.Engine(10_000.0, 0.0004)
    engine.load_ohlcv(
        {
            "ts": np.array(TS_NS, dtype=np.int64),
            "open": np.array([100.0, 100.5], dtype=np.float64),
            "high": np.array([101.0, 102.0], dtype=np.float64),
            "low": np.array([99.0, 100.0], dtype=np.float64),
            "close": np.array([100.5, 101.5], dtype=np.float64),
            "volume": np.array([3.0, 4.0], dtype=np.float64),
        }
    )
    return engine


class EngineTimestampDtype(unittest.TestCase):
    def test_ts_dtype_is_int64(self) -> None:
        engine = _engine_with_bars()
        self.assertEqual(engine.ts().dtype, np.dtype(np.int64),
                         "a nanosecond timestamp is an integer, not a float")

    def test_ts_round_trips_above_two_to_the_53(self) -> None:
        engine = _engine_with_bars()
        got = engine.ts()
        self.assertEqual(len(got), len(TS_NS))
        for i, expected in enumerate(TS_NS):
            self.assertEqual(int(got[i]), expected,
                             f"timestamp {i} lost its low digits on the way out")

    def test_ohlcv_columns_stay_float64(self) -> None:
        engine = _engine_with_bars()
        for name in ("open", "high", "low", "close", "volume"):
            arr = getattr(engine, name)()
            self.assertEqual(arr.dtype, np.dtype(np.float64),
                             f"{name}() is a converted price, and stays float64")

    def test_signals_timestamped_from_ts_still_run(self) -> None:
        # SignalBuilder already takes int64 nanoseconds, so feeding it the
        # values Engine.ts() hands back has to keep working -- and does
        # not, if ts() rounds them onto a different bar.
        engine = _engine_with_bars()
        ts = engine.ts()
        signals = flox.SignalBuilder()
        signals.buy(int(ts[0]), 0.01)
        signals.sell(int(ts[1]), 0.01)
        stats = engine.run(signals)
        self.assertEqual(stats.total_trades, 1,
                         "buy on the first bar, sell on the second -- one closed round trip")


if __name__ == "__main__":
    unittest.main()
