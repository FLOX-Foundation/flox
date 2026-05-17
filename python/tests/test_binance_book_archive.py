"""Tests for ``flox_py.archives.binance.{t1_to_floxlog, depth20_to_floxlog}``.

No network: synthetic bookTicker and bookDepth CSVs matching the
exact column layouts published on ``data.binance.vision`` are built
in-memory and fed through the converters. The resulting `.floxlog`
directory is read back via ``flox_py.DataReader.read_book_updates``
and asserted ladder-by-ladder.

Covers:

  * `t1_to_floxlog`: initial snapshot + delta on the next change, no-op
    on unchanged ticks, dedup on re-run by `update_id`.
  * `depth20_to_floxlog`: long-format snap → 20-level snapshot,
    subsequent updates → deltas with qty=0 removals + new prices.
  * Co-existence: aggTrades + book events sit in one tape, monotonic
    on `exchange_ts_ns` (the existing reader interleaves them).
  * `metadata.json`: `has_book_snapshots` / `has_book_deltas` /
    `total_book_updates` / `book_depth` populated by the writer.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_binance_book_archive.py
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import flox_py  # noqa: E402

from flox_py.archives import binance as bnc  # noqa: E402


# bookTicker rows: update_id, bid_p, bid_q, ask_p, ask_q,
# transaction_time, event_time. Three rows: one initial, one
# unchanged, one with new top.
_T1_ROWS = [
    (100, 42_000.0, 1.0, 42_001.0, 1.0, 1_700_000_000_000, 1_700_000_000_000),
    (101, 42_000.0, 1.0, 42_001.0, 1.0, 1_700_000_001_000, 1_700_000_001_000),
    (102, 41_999.0, 2.0, 42_002.0, 1.5, 1_700_000_002_000, 1_700_000_002_000),
]


def _build_t1_zip(dest: Path) -> Path:
    buf = io.StringIO()
    for r in _T1_ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def _build_depth20_zip(dest: Path) -> Path:
    """bookDepth long format: symbol, ts_ms, first_id, last_id, side,
    update_type, price, qty.

    First group (last_update_id=10) is the snapshot — 3 bids + 3 asks.
    Second group (last_update_id=11) is an update: top bid removed,
    new top bid added, top ask qty changed.
    """
    rows = [
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 42_000.0, 1.0),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 41_999.0, 1.5),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 41_998.0, 2.0),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_001.0, 1.0),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_002.0, 1.5),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_003.0, 2.0),
        ("BTCUSDT", 1_700_000_001_000, 11, 11, "b", "set",  42_000.0, 0.0),
        ("BTCUSDT", 1_700_000_001_000, 11, 11, "b", "set",  42_000.5, 0.8),
        ("BTCUSDT", 1_700_000_001_000, 11, 11, "a", "set",  42_001.0, 0.5),
    ]
    buf = io.StringIO()
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def _read_events(tape_dir: Path):
    """Return list of dict(seq, ts_ns, is_snapshot, bids, asks)."""
    r = flox_py.DataReader(str(tape_dir))
    headers, levels = r.read_book_updates()
    events = []
    for h in headers:
        off = int(h["level_offset"])
        n_bid = int(h["bid_count"])
        n_ask = int(h["ask_count"])
        bids = [(int(levels[off + i]["price_raw"]),
                 int(levels[off + i]["qty_raw"]))
                for i in range(n_bid)]
        asks = [(int(levels[off + n_bid + i]["price_raw"]),
                 int(levels[off + n_bid + i]["qty_raw"]))
                for i in range(n_ask)]
        events.append(dict(
            seq=int(h["seq"]),
            ts_ns=int(h["exchange_ts_ns"]),
            symbol_id=int(h["symbol_id"]),
            # PyDataReader.readBookUpdates maps event_type from
            # book_header.type (0 = snapshot, 1 = delta).
            is_snapshot=int(h["event_type"]) == 0,
            bids=bids, asks=asks,
        ))
    return events


class T1Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-t1-"))
        self.zip_path = self.tmp / "BTCUSDT-bookTicker-2024-01-15.zip"
        self.tape = self.tmp / "tape"

    def test_initial_snapshot_then_delta(self) -> None:
        _build_t1_zip(self.zip_path)
        stats = bnc.t1_to_floxlog(
            self.zip_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="um-futures",
        )
        self.assertEqual(stats.snapshots_written, 1)
        self.assertEqual(stats.deltas_written, 1)
        self.assertEqual(stats.rows_skipped, 1)  # the unchanged tick

        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)
        self.assertTrue(events[0]["is_snapshot"])
        self.assertFalse(events[1]["is_snapshot"])
        # Snapshot top-of-book matches the first row exactly.
        self.assertEqual(events[0]["bids"], [(4_200_000_000_000, 100_000_000)])
        self.assertEqual(events[0]["asks"], [(4_200_100_000_000, 100_000_000)])
        # Delta includes both removals (qty=0) and the new top.
        # Order is implementation-defined; check as sets.
        self.assertEqual(set(events[1]["bids"]),
                         {(4_199_900_000_000, 200_000_000),
                          (4_200_000_000_000, 0)})
        self.assertEqual(set(events[1]["asks"]),
                         {(4_200_200_000_000, 150_000_000),
                          (4_200_100_000_000, 0)})

    def test_dedup_on_rerun(self) -> None:
        _build_t1_zip(self.zip_path)
        bnc.t1_to_floxlog(self.zip_path, self.tape,
                          symbol_id=7, symbol_name="BTCUSDT",
                          market="um-futures")
        second = bnc.t1_to_floxlog(
            self.zip_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="um-futures",
        )
        # Every row's update_id <= last_seq → all skipped.
        self.assertEqual(second.snapshots_written, 0)
        self.assertEqual(second.deltas_written, 0)
        self.assertEqual(second.rows_skipped, 3)
        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)  # unchanged from first run


class Depth20Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-d20-"))
        self.zip_path = self.tmp / "BTCUSDT-bookDepth-2024-01-15.zip"
        self.tape = self.tmp / "tape"

    def test_snapshot_then_delta(self) -> None:
        _build_depth20_zip(self.zip_path)
        stats = bnc.depth20_to_floxlog(
            self.zip_path, self.tape, levels=20,
            symbol_id=7, symbol_name="BTCUSDT",
        )
        self.assertEqual(stats.snapshots_written, 1)
        self.assertEqual(stats.deltas_written, 1)

        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)
        # Snapshot: 3 bids + 3 asks.
        self.assertTrue(events[0]["is_snapshot"])
        self.assertEqual(len(events[0]["bids"]), 3)
        self.assertEqual(len(events[0]["asks"]), 3)
        # Delta: top bid removed, new top bid added, top ask changed.
        self.assertFalse(events[1]["is_snapshot"])
        bids = dict(events[1]["bids"])
        asks = dict(events[1]["asks"])
        # 42_000.0 → qty 0 (removal); 42_000.5 → 0.8 (new).
        self.assertEqual(bids[4_200_000_000_000], 0)
        self.assertEqual(bids[4_200_050_000_000], 80_000_000)
        # 42_001.0 ask qty changed 1.0 → 0.5.
        self.assertEqual(asks[4_200_100_000_000], 50_000_000)

    def test_metadata_book_fields(self) -> None:
        _build_depth20_zip(self.zip_path)
        bnc.depth20_to_floxlog(self.zip_path, self.tape, levels=20,
                               symbol_id=7, symbol_name="BTCUSDT")
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertTrue(meta["has_book_snapshots"])
        self.assertTrue(meta["has_book_deltas"])
        self.assertEqual(meta["total_book_updates"], 2)
        self.assertEqual(meta["book_depth"], 20)


class CoexistenceTests(unittest.TestCase):
    """aggTrades + book events in one tape — both readable via the
    existing reader surfaces, monotonic on exchange_ts_ns when
    interleaved (DataReader sorts by ts_ns on read)."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-coex-"))
        self.tape = self.tmp / "tape"

    def test_trades_and_books_share_tape(self) -> None:
        # aggTrades archive
        agg_zip = self.tmp / "BTCUSDT-aggTrades-2024-01-15.zip"
        buf = io.StringIO()
        # (agg_id, price, qty, first_id, last_id, ts_ms, maker, best)
        for i in range(3):
            buf.write(f"{1000+i},42000.0,0.01,{i},{i},"
                      f"{1_700_000_000_500 + i * 1000},True,True\n")
        with zipfile.ZipFile(agg_zip, "w") as z:
            z.writestr(agg_zip.with_suffix(".csv").name, buf.getvalue())

        # bookTicker archive
        bt_zip = self.tmp / "BTCUSDT-bookTicker-2024-01-15.zip"
        _build_t1_zip(bt_zip)

        bnc.aggtrades_to_floxlog(agg_zip, self.tape, symbol_id=1,
                                 symbol_name="BTCUSDT", market="um-futures")
        bnc.t1_to_floxlog(bt_zip, self.tape, symbol_id=1,
                          symbol_name="BTCUSDT", market="um-futures")

        # Trade rows readable.
        r = flox_py.DataReader(str(self.tape))
        trades = r.read_trades()
        self.assertEqual(int(trades.size), 3)
        # Book events readable.
        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)
        # Metadata reflects both streams.
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertTrue(meta["has_trades"])
        self.assertTrue(meta["has_book_snapshots"])
        self.assertGreaterEqual(meta["total_trades"], 3)
        self.assertGreaterEqual(meta["total_book_updates"], 2)


if __name__ == "__main__":
    unittest.main()
