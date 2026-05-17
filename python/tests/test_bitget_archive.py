"""Tests for ``flox_py.archives.bitget`` — Bitget public archive converter.

Synthetic CSV matching Bitget's column layout (`trade_id, price,
size, side, timestamp_ms`) is built in memory and round-tripped
through the converter.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_bitget_archive.py
"""
from __future__ import annotations

import gzip
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

from flox_py.archives import ArchiveReader, bitget  # noqa: E402


# Bitget columns: trade_id, price, size, side, timestamp_ms.
_ROWS = [
    (100, 42_100.5, 0.01, "buy",  1_700_000_000_500),
    (101, 42_101.0, 0.02, "sell", 1_700_000_001_250),
    (102, 42_100.7, 0.03, "buy",  1_700_000_002_000),
]


def _csv_text() -> str:
    buf = io.StringIO()
    buf.write("trade_id,price,size,side,timestamp_ms\n")
    for r in _ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    return buf.getvalue()


def _build_zip(dest: Path) -> Path:
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, _csv_text())
    return dest


def _build_gz(dest: Path) -> Path:
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(_csv_text())
    return dest


class BitgetConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bitget-"))
        self.tape = self.tmp / "tape"

    def _convert_zip(self) -> bitget.ConvertStats:
        z = self.tmp / "BTCUSDT-trades-2024-01-15.zip"
        _build_zip(z)
        return bitget.trades_to_floxlog(
            z, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="umcbl",
        )

    def test_round_trip_zip(self) -> None:
        stats = self._convert_zip()
        self.assertEqual(stats.trades_written, 3)
        trades = flox_py.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 3)
        sides = [int(t["side"]) for t in trades]
        self.assertEqual(sides, [0, 1, 0])
        ts_ns = [int(t["exchange_ts_ns"]) for t in trades]
        self.assertEqual(ts_ns,
                         [1_700_000_000_500_000_000,
                          1_700_000_001_250_000_000,
                          1_700_000_002_000_000_000])

    def test_round_trip_gz(self) -> None:
        g = self.tmp / "BTCUSDT-trades-2024-01-15.csv.gz"
        _build_gz(g)
        stats = bitget.trades_to_floxlog(
            g, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="umcbl",
        )
        self.assertEqual(stats.trades_written, 3)

    def test_dedup_on_rerun(self) -> None:
        first = self._convert_zip()
        self.assertEqual(first.trades_written, 3)
        second = bitget.trades_to_floxlog(
            self.tmp / "BTCUSDT-trades-2024-01-15.zip", self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="umcbl",
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)

    def test_metadata(self) -> None:
        self._convert_zip()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "bitget")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTCUSDT")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_spot_market_metadata(self) -> None:
        z = self.tmp / "BTCUSDT-trades-2024-01-15.zip"
        _build_zip(z)
        bitget.trades_to_floxlog(
            z, self.tape, symbol_id=7,
            symbol_name="BTCUSDT", market="spot",
        )
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["instrument_type"], "spot")

    def test_protocol_compliance(self) -> None:
        self.assertIsInstance(bitget, ArchiveReader)


if __name__ == "__main__":
    unittest.main()
