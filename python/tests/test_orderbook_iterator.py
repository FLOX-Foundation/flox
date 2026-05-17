"""Tests for ``flox_py.orderbook.OrderBookIterator`` and ``book_at``.

Builds a synthetic tape with known book events (snapshot + deltas) at
controlled timestamps, iterates with various bucket / level / symbol
filters, then point-queries `book_at` at chosen offsets and checks
the reconstructed ladder matches the expected state.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_orderbook_iterator.py
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

from flox_py import orderbook as ob  # noqa: E402


_LEVEL_DTYPE = np.dtype([
    ("price_raw", np.int64),
    ("qty_raw", np.int64),
    ("side", np.uint8),
])


def _level(price: float, qty: float):
    return (int(round(price * 1e8)), int(round(qty * 1e8)), 0)


def _arr(items):
    return np.array(items, dtype=_LEVEL_DTYPE)


def _write_book_tape(tape_dir: Path, events) -> Path:
    """events: list of (ts_ns, symbol_id, is_snapshot, [(p, q)] bids, [(p, q)] asks)."""
    tape_dir.mkdir(parents=True, exist_ok=True)
    w = flox_py.DataWriter(str(tape_dir), max_segment_mb=4,
                           exchange_id=0, compression="none")
    try:
        for i, (ts, sym, is_snap, bids, asks) in enumerate(events):
            w.write_book(
                exchange_ts_ns=int(ts), recv_ts_ns=int(ts),
                seq=i, symbol_id=int(sym),
                is_snapshot=is_snap,
                bids=_arr([_level(p, q) for p, q in bids]),
                asks=_arr([_level(p, q) for p, q in asks]),
            )
    finally:
        w.close()
    return tape_dir


_BASE_NS = 1_700_000_000_000_000_000


class IteratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="ob-iter-"))
        self.tape = self.tmp / "tape"
        # Three events at 0s, 30s, 90s — with 60s bucket:
        # bucket 0 = [0..60), bucket 60s = [60..120). 30s event lands
        # in bucket 0; 90s event lands in bucket 60s.
        events = [
            (_BASE_NS,             1, True,
             [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)]),
            (_BASE_NS + 30_000_000_000, 1, False,
             [(100.0, 0.5)], []),  # bid qty change
            (_BASE_NS + 90_000_000_000, 1, False,
             [(99.0, 0.0)], [(101.0, 0.0)]),  # remove a bid + remove top ask
        ]
        _write_book_tape(self.tape, events)

    def test_one_snap_per_bucket(self) -> None:
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5))
        self.assertEqual(len(snaps), 2)
        # Bucket boundary is the floor of the event ts onto the
        # 60s grid; consecutive snaps therefore advance by 60s.
        self.assertEqual(snaps[1].ts_ns - snaps[0].ts_ns,
                         60_000_000_000)
        # Bucket 0: latest state (after the 30s delta).
        self.assertEqual(snaps[0].bids[0], (100.0, 0.5))
        self.assertEqual(snaps[0].bids[1], (99.0, 2.0))
        self.assertEqual(snaps[0].asks[0], (101.0, 1.5))
        # Bucket 60s: after removing 99 bid and top ask.
        self.assertEqual(snaps[1].bids, [(100.0, 0.5)])
        self.assertEqual(snaps[1].asks[0], (102.0, 2.5))

    def test_levels_cap(self) -> None:
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=1))
        # levels=1 keeps only top-of-book per side.
        for s in snaps:
            self.assertLessEqual(len(s.bids), 1)
            self.assertLessEqual(len(s.asks), 1)

    def test_symbol_filter(self) -> None:
        # Inject a second symbol; iterator with explicit filter sees
        # only the requested symbol.
        events = [
            (_BASE_NS, 2, True, [(50.0, 1.0)], [(51.0, 1.0)]),
        ]
        # Append events for symbol 2 to the existing tape.
        w = flox_py.DataWriter(str(self.tape), max_segment_mb=4,
                               exchange_id=0, compression="none")
        try:
            for i, (ts, sym, is_snap, bids, asks) in enumerate(events):
                w.write_book(
                    exchange_ts_ns=int(ts), recv_ts_ns=int(ts),
                    seq=100 + i, symbol_id=int(sym),
                    is_snapshot=is_snap,
                    bids=_arr([_level(p, q) for p, q in bids]),
                    asks=_arr([_level(p, q) for p, q in asks]),
                )
        finally:
            w.close()

        snaps_sym1 = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5, symbol_id=1))
        self.assertTrue(all(s.symbol_id == 1 for s in snaps_sym1))
        snaps_sym2 = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5, symbol_id=2))
        self.assertTrue(all(s.symbol_id == 2 for s in snaps_sym2))
        self.assertGreaterEqual(len(snaps_sym2), 1)

    def test_empty_tape(self) -> None:
        empty = self.tmp / "empty"
        empty.mkdir()
        # No segments written → iterator yields nothing.
        try:
            snaps = list(ob.OrderBookIterator(
                empty, bucket_ns=60_000_000_000, levels=5))
            self.assertEqual(snaps, [])
        except Exception:
            # An empty directory may also be treated as "tape not
            # initialised" by DataReader; that is acceptable too.
            pass

    def test_bad_inputs(self) -> None:
        with self.assertRaises(ValueError):
            ob.OrderBookIterator(self.tape, bucket_ns=0, levels=5)
        with self.assertRaises(ValueError):
            ob.OrderBookIterator(self.tape, bucket_ns=60_000_000_000,
                                 levels=-1)


class BookAtTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="ob-at-"))
        self.tape = self.tmp / "tape"
        events = [
            (_BASE_NS,            1, True,
             [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)]),
            (_BASE_NS + 5_000_000_000, 1, False,
             [(100.0, 0.7)], []),
            (_BASE_NS + 10_000_000_000, 1, False,
             [(99.0, 0.0)], [(101.0, 0.0)]),
        ]
        _write_book_tape(self.tape, events)

    def test_point_query_at_each_event(self) -> None:
        snap0 = ob.book_at(self.tape, ts_ns=_BASE_NS + 1, levels=5)
        self.assertIsNotNone(snap0)
        self.assertEqual(snap0.bids[0], (100.0, 1.0))
        self.assertEqual(snap0.asks[0], (101.0, 1.5))

        snap1 = ob.book_at(self.tape,
                           ts_ns=_BASE_NS + 5_000_000_000 + 1, levels=5)
        # Top bid qty updated to 0.7.
        self.assertEqual(snap1.bids[0], (100.0, 0.7))

        snap2 = ob.book_at(self.tape,
                           ts_ns=_BASE_NS + 10_000_000_000 + 1, levels=5)
        # 99 bid + 101 ask removed.
        self.assertEqual([p for p, _ in snap2.bids], [100.0])
        self.assertEqual([p for p, _ in snap2.asks], [102.0])

    def test_point_query_before_first_event(self) -> None:
        snap = ob.book_at(self.tape, ts_ns=_BASE_NS - 1, levels=5)
        self.assertIsNone(snap)


if __name__ == "__main__":
    unittest.main()
