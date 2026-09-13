"""Ordering and boundary contract of ``flox_py.DataReader``.

Covers what a tape reader owes its caller when the tape is not perfectly
ordered: late events are dropped and counted rather than ending the walk,
strict mode restores the raise, a symbol that is not being replayed does not
move the ordering watermark, and the two time bounds (inclusive for trades,
exclusive for option quotes) stay where they are documented.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_replay_reader_ordering.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import flox_py  # noqa: E402

_SEC = 1_000_000_000
_BASE = 1_700_000_000 * _SEC


def _write_lagging_tape(tape_dir: Path) -> None:
    """50 in-order trades on symbol 1, one trade on symbol 2 stamped 40s back,
    then 50 more on symbol 1."""
    tape_dir.mkdir(parents=True, exist_ok=True)
    w = flox_py.DataWriter(str(tape_dir), max_segment_mb=4,
                           exchange_id=0, compression="none")
    try:
        for i in range(50):
            w.write_trade(exchange_ts_ns=_BASE + i * _SEC, recv_ts_ns=_BASE + i * _SEC,
                          price=100.0, qty=1.0, trade_id=i, symbol_id=1, side=0)
        w.write_trade(exchange_ts_ns=_BASE + 9 * _SEC, recv_ts_ns=_BASE + 9 * _SEC,
                      price=100.0, qty=1.0, trade_id=999, symbol_id=2, side=0)
        for i in range(50, 100):
            w.write_trade(exchange_ts_ns=_BASE + i * _SEC, recv_ts_ns=_BASE + i * _SEC,
                          price=100.0, qty=1.0, trade_id=i, symbol_id=1, side=0)
    finally:
        w.close()


class OrderingTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="replay-order-")
        self.tape = Path(self._tmp.name) / "tape"
        _write_lagging_tape(self.tape)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_late_event_is_dropped_and_counted(self) -> None:
        reader = flox_py.DataReader(str(self.tape), reorder_window_ns=10 * _SEC)
        agg = flox_py.EventTypeStatsAggregator()
        self.assertTrue(reader.run([agg], n_threads=1))

        stats = reader.stats()
        self.assertEqual(stats["late_dropped"], 1)
        self.assertEqual(stats["events_read"], 100)

    def test_strict_ordering_raises(self) -> None:
        reader = flox_py.DataReader(str(self.tape), reorder_window_ns=10 * _SEC,
                                    strict_ordering=True)
        agg = flox_py.EventTypeStatsAggregator()
        with self.assertRaises(Exception) as ctx:
            reader.run([agg], n_threads=1)
        # The message has to say which symbol and where, or the caller cannot
        # find the fifteen frames out of twelve million that caused it.
        message = str(ctx.exception)
        self.assertIn("symbol_id=2", message)
        self.assertIn("offset=", message)

    def test_filtered_symbol_does_not_move_the_watermark(self) -> None:
        reader = flox_py.DataReader(str(self.tape), symbols=[1],
                                    reorder_window_ns=10 * _SEC)
        agg = flox_py.EventTypeStatsAggregator()
        self.assertTrue(reader.run([agg], n_threads=1))

        stats = reader.stats()
        self.assertEqual(stats["events_read"], 100)
        self.assertEqual(stats["late_dropped"], 0)

    def test_wide_window_keeps_every_event(self) -> None:
        reader = flox_py.DataReader(str(self.tape), reorder_window_ns=60 * _SEC)
        agg = flox_py.EventTypeStatsAggregator()
        self.assertTrue(reader.run([agg], n_threads=1))

        stats = reader.stats()
        self.assertEqual(stats["late_dropped"], 0)
        self.assertEqual(stats["events_read"], 101)


class TimeBoundTests(unittest.TestCase):
    """The two readers on one object treat `to_ns` differently. The difference
    is deliberate and pinned here so it cannot drift unnoticed: trades are
    inclusive, option quotes are exclusive so that day-by-day slicing of a
    month returns each quote exactly once."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="replay-bounds-")
        self.tape = Path(self._tmp.name) / "tape"
        self.tape.mkdir(parents=True, exist_ok=True)
        stamps = np.array([_BASE + i * _SEC for i in range(10)], dtype=np.int64)
        w = flox_py.DataWriter(str(self.tape), max_segment_mb=4,
                               exchange_id=0, compression="none")
        try:
            for i, ts in enumerate(stamps):
                w.write_trade(exchange_ts_ns=int(ts), recv_ts_ns=int(ts),
                              price=100.0, qty=1.0,
                              trade_id=i, symbol_id=1, side=0)
            ones = np.ones(len(stamps), dtype=np.float64)
            w.write_option_quotes(
                exchange_ts_ns=stamps,
                recv_ts_ns=stamps,
                mark_prices=ones * 100.0,
                index_prices=ones * 100.0,
                ivs=ones * 0.65,
                open_interest=ones,
                symbol_ids=np.ones(len(stamps), dtype=np.uint32),
            )
        finally:
            w.close()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_trade_upper_bound_is_inclusive(self) -> None:
        boundary = _BASE + 4 * _SEC
        reader = flox_py.DataReader(str(self.tape), to_ns=boundary)
        trades = reader.read_trades()
        self.assertEqual(len(trades), 5)
        self.assertEqual(int(trades[-1]["exchange_ts_ns"]), boundary)

    def test_option_quote_upper_bound_is_exclusive(self) -> None:
        boundary = _BASE + 4 * _SEC
        reader = flox_py.DataReader(str(self.tape), to_ns=boundary)
        quotes = reader.read_option_quotes_from(_BASE)
        self.assertEqual(len(quotes), 4)
        self.assertLess(int(quotes[-1]["exchange_ts_ns"]), boundary)

    def test_adjacent_quote_windows_do_not_overlap(self) -> None:
        split = _BASE + 5 * _SEC
        first = flox_py.DataReader(str(self.tape), to_ns=split).read_option_quotes_from(_BASE)
        second = flox_py.DataReader(str(self.tape)).read_option_quotes_from(split)
        stamps = [int(q["exchange_ts_ns"]) for q in first]
        stamps += [int(q["exchange_ts_ns"]) for q in second]
        self.assertEqual(len(stamps), len(set(stamps)))
        self.assertEqual(len(stamps), 10)


if __name__ == "__main__":
    unittest.main()
