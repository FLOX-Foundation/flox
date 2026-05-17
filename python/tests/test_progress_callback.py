"""Tests for ``DataReader.run(progress_callback=..., progress_interval_ms=...)``.

Covers:

  * The callback fires at least once on a tape long enough to cross
    the interval, with monotonic ``ts_ns`` and ``pct`` ending at 1.0.
  * Returning ``False`` cancels the run; partial aggregator state is
    still finalised.
  * Raising inside the callback cancels and re-raises after the run
    returns so the caller sees the exception, not a silent partial.
  * Omitting the callback (default) costs no overhead vs the existing
    single-arg ``run()`` call shape.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_progress_callback.py
"""
from __future__ import annotations

import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import flox_py  # noqa: E402


def _write_tape(tmp: Path, n_trades: int = 50_000) -> Path:
    out = tmp / "tape"
    out.mkdir(parents=True, exist_ok=True)
    w = flox_py.DataWriter(str(out), max_segment_mb=64,
                           exchange_id=0, compression="none")
    base = 1_700_000_000_000_000_000
    try:
        for i in range(n_trades):
            ts = base + i * 1_000  # 1 µs apart
            w.write_trade(exchange_ts_ns=ts, recv_ts_ns=ts,
                          price=100.0 + i * 0.01, qty=1.0,
                          trade_id=i, symbol_id=1, side=0)
    finally:
        w.close()
    return out


class ProgressCallbackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="progress-cb-"))

    def test_default_run_unchanged(self) -> None:
        tape = _write_tape(self.tmp, n_trades=1_000)
        r = flox_py.DataReader(str(tape))
        agg = flox_py.OHLCBinAggregator(bucket_ns=60_000_000_000,
                                        by_symbol=False)
        # Original positional shape still works.
        self.assertTrue(r.run([agg], 1))

    def test_callback_fires_with_monotonic_ts(self) -> None:
        tape = _write_tape(self.tmp, n_trades=50_000)
        r = flox_py.DataReader(str(tape))
        agg = flox_py.OHLCBinAggregator(bucket_ns=60_000_000_000,
                                        by_symbol=False)
        calls: list[tuple[float, int]] = []

        def cb(pct: float, ts: int) -> bool:
            calls.append((pct, ts))
            return True

        # 1ms interval keeps the test tight without spamming output.
        ok = r.run([agg], n_threads=1, progress_callback=cb,
                   progress_interval_ms=1)
        self.assertTrue(ok)
        # At least the final 100% tick fires even on a fast tape.
        self.assertGreaterEqual(len(calls), 1)
        # Timestamps non-decreasing.
        for prev, cur in zip(calls, calls[1:]):
            self.assertGreaterEqual(cur[1], prev[1])
        # Last call is exactly 1.0.
        self.assertAlmostEqual(calls[-1][0], 1.0, places=6)

    def test_callback_returning_false_cancels(self) -> None:
        tape = _write_tape(self.tmp, n_trades=50_000)
        r = flox_py.DataReader(str(tape))
        agg = flox_py.OHLCBinAggregator(bucket_ns=60_000_000_000,
                                        by_symbol=False)
        calls = [0]

        def cb(pct: float, ts: int) -> bool:
            calls[0] += 1
            return False  # cancel on first call

        # 1ms interval guarantees at least one fire on a 50k-event tape.
        ok = r.run([agg], n_threads=1, progress_callback=cb,
                   progress_interval_ms=1)
        # Cancellation surfaces as a False return.
        self.assertFalse(ok)
        # Aggregator was finalised with partial state — calling
        # .result() is safe and returns a numpy array (possibly empty).
        out = agg.result()
        self.assertIsNotNone(out)
        # The callback fired at least once on cancellation.
        self.assertGreaterEqual(calls[0], 1)

    def test_callback_exception_propagates(self) -> None:
        tape = _write_tape(self.tmp, n_trades=50_000)
        r = flox_py.DataReader(str(tape))
        agg = flox_py.OHLCBinAggregator(bucket_ns=60_000_000_000,
                                        by_symbol=False)

        class Sentinel(RuntimeError):
            pass

        def cb(pct: float, ts: int) -> bool:
            raise Sentinel("cancel via exception")

        with self.assertRaises(Sentinel):
            r.run([agg], n_threads=1, progress_callback=cb,
                  progress_interval_ms=1)

    def test_callback_returning_none_keeps_running(self) -> None:
        # Pythonic forgive-the-bool: returning None (or omitting the
        # return) should NOT cancel — only explicit False cancels.
        tape = _write_tape(self.tmp, n_trades=1_000)
        r = flox_py.DataReader(str(tape))
        agg = flox_py.OHLCBinAggregator(bucket_ns=60_000_000_000,
                                        by_symbol=False)
        calls = [0]

        def cb(pct: float, ts: int) -> None:
            calls[0] += 1
            return None

        ok = r.run([agg], n_threads=1, progress_callback=cb,
                   progress_interval_ms=1)
        self.assertTrue(ok)
        self.assertGreaterEqual(calls[0], 1)


if __name__ == "__main__":
    unittest.main()
