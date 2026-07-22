"""Tests for ``flox_py.BookSnapshotBinAggregator``.

Parity contract: on the same tape, the C++ aggregator run through
``DataReader.run([...], n_threads=1)`` must reproduce exactly the
snapshots that ``flox_py.orderbook.OrderBookIterator`` yields —
same buckets, same levels, same crossed flag. Plus aggregator-only
semantics: zero-padding of the shorter side and the parallel-run
refusal.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_book_snapshot_aggregator.py
"""
from __future__ import annotations

import random
import sys
import tempfile
import unittest
from collections import defaultdict
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

_BASE_NS = 1_700_000_000_000_000_000
_BUCKET = 60_000_000_000


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


def _run_aggregator(tape: Path, *, levels: int):
    agg = flox_py.BookSnapshotBinAggregator(bucket_ns=_BUCKET, levels=levels)
    reader = flox_py.DataReader(str(tape))
    self_ok = reader.run([agg], n_threads=1)
    assert self_ok
    return agg.result()


def _rows_to_snapshots(rows):
    """Group aggregator rows into {(bucket_ts, symbol): (bids, asks, crossed)}
    with float (price, qty) lists matching BookSnapshot conventions."""
    cells = defaultdict(lambda: ([], [], False))
    for r in rows:
        key = (int(r["bucket_ts_ns"]), int(r["symbol_id"]))
        bids, asks, _ = cells[key]
        crossed = bool(int(r["flags"]) & 1)
        if int(r["bid_price_raw"]) or int(r["bid_qty_raw"]):
            bids.append((int(r["bid_price_raw"]) / 1e8,
                         int(r["bid_qty_raw"]) / 1e8))
        if int(r["ask_price_raw"]) or int(r["ask_qty_raw"]):
            asks.append((int(r["ask_price_raw"]) / 1e8,
                         int(r["ask_qty_raw"]) / 1e8))
        cells[key] = (bids, asks, crossed)
    return dict(cells)


def _iterator_snapshots(tape: Path, *, levels: int):
    out = {}
    for snap in ob.OrderBookIterator(tape, bucket_ns=_BUCKET, levels=levels):
        out[(snap.ts_ns, snap.symbol_id)] = (snap.bids, snap.asks, snap.crossed)
    return out


class ParityTests(unittest.TestCase):
    def test_handcrafted_parity(self) -> None:
        tmp = Path(tempfile.mkdtemp(prefix="bsagg-"))
        tape = tmp / "tape"
        events = [
            (_BASE_NS, 1, True,
             [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)]),
            (_BASE_NS + 30_000_000_000, 1, False, [(100.0, 0.5)], []),
            (_BASE_NS + 90_000_000_000, 1, False,
             [(99.0, 0.0)], [(101.0, 0.0)]),
            (_BASE_NS + 100_000_000_000, 2, True,
             [(50.0, 1.0)], [(51.0, 1.0)]),
        ]
        _write_book_tape(tape, events)

        got = _rows_to_snapshots(_run_aggregator(tape, levels=5))
        want = _iterator_snapshots(tape, levels=5)
        self.assertEqual(got, want)

    def test_randomized_parity(self) -> None:
        rng = random.Random(20260717)
        tmp = Path(tempfile.mkdtemp(prefix="bsagg-rand-"))
        tape = tmp / "tape"
        events = []
        ts = _BASE_NS
        for sym in (1, 2):
            events.append((ts, sym, True,
                           [(100.0 + i, 1.0) for i in range(8)],
                           [(120.0 + i, 1.0) for i in range(8)]))
            ts += 1
        for _ in range(2000):
            ts += rng.randrange(1, 20_000_000_000)
            sym = rng.choice((1, 2))
            if rng.random() < 0.05:
                events.append((ts, sym, True,
                               [(100.0 + rng.randrange(20), rng.uniform(0.1, 5))
                                for _ in range(rng.randrange(1, 6))],
                               [(120.0 + rng.randrange(20), rng.uniform(0.1, 5))
                                for _ in range(rng.randrange(1, 6))]))
            else:
                bids = [(100.0 + rng.randrange(20),
                         0.0 if rng.random() < 0.3 else rng.uniform(0.1, 5))
                        for _ in range(rng.randrange(0, 4))]
                asks = [(120.0 + rng.randrange(20),
                         0.0 if rng.random() < 0.3 else rng.uniform(0.1, 5))
                        for _ in range(rng.randrange(0, 4))]
                events.append((ts, sym, False, bids, asks))
        _write_book_tape(tape, events)

        for levels in (1, 3, 20):
            got = _rows_to_snapshots(_run_aggregator(tape, levels=levels))
            want = _iterator_snapshots(tape, levels=levels)
            # The iterator may yield snapshots with both sides empty;
            # the aggregator skips those cells by design.
            want = {k: v for k, v in want.items() if v[0] or v[1]}
            self.assertEqual(got, want, f"parity broke at levels={levels}")


class AggregatorSemanticsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bsagg-sem-"))
        self.tape = self.tmp / "tape"
        _write_book_tape(self.tape, [
            (_BASE_NS, 1, True,
             [(100.0, 1.0), (99.0, 1.0), (98.0, 1.0)], [(101.0, 1.0)]),
        ])

    def test_zero_padding_and_levels_cap(self) -> None:
        rows = _run_aggregator(self.tape, levels=2)
        self.assertEqual(len(rows), 2)
        self.assertEqual(int(rows[0]["level"]), 0)
        self.assertEqual(int(rows[1]["level"]), 1)
        # Second row has a bid but no ask at that depth.
        self.assertEqual(int(rows[1]["bid_price_raw"]), int(99.0 * 1e8))
        self.assertEqual(int(rows[1]["ask_price_raw"]), 0)
        self.assertEqual(int(rows[1]["ask_qty_raw"]), 0)

    def test_small_tape_n_threads_falls_back_single(self) -> None:
        # The reader parallelizes intra-segment only when a segment has
        # enough compressed blocks; small/uncompressed tapes run on the
        # master panel regardless of n_threads, so this must succeed.
        # On real multi-block tapes a parallel run calls cloneEmpty,
        # which raises by design (covered by the C++ unit test).
        agg = flox_py.BookSnapshotBinAggregator(bucket_ns=_BUCKET, levels=5)
        reader = flox_py.DataReader(str(self.tape))
        self.assertTrue(reader.run([agg], n_threads=2))
        self.assertEqual(len(agg.result()), 3)

    def test_combines_with_trade_aggregators_in_one_pass(self) -> None:
        # A trade in the tape alongside books: one run() feeds both
        # aggregator kinds.
        w = flox_py.DataWriter(str(self.tape), max_segment_mb=4,
                               exchange_id=0, compression="none")
        try:
            w.write_trade(exchange_ts_ns=_BASE_NS + 1_000, recv_ts_ns=_BASE_NS + 1_000,
                          price=100.5, qty=1.0, trade_id=1, symbol_id=1, side=0)
        finally:
            w.close()
        book_agg = flox_py.BookSnapshotBinAggregator(bucket_ns=_BUCKET, levels=5)
        ohlc = flox_py.OHLCBinAggregator(bucket_ns=_BUCKET, by_symbol=True)
        reader = flox_py.DataReader(str(self.tape))
        self.assertTrue(reader.run([book_agg, ohlc], n_threads=1))
        self.assertEqual(len(book_agg.result()), 3)  # 3 bid levels
        self.assertEqual(len(ohlc.result()), 1)

    def test_bad_args(self) -> None:
        with self.assertRaises(Exception):
            flox_py.BookSnapshotBinAggregator(bucket_ns=0, levels=5)
        with self.assertRaises(Exception):
            flox_py.BookSnapshotBinAggregator(bucket_ns=_BUCKET, levels=0)


if __name__ == "__main__":
    unittest.main()
