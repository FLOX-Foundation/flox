"""Tests for ``flox_py.archives.deribit`` — Deribit public archive
converter (perpetual / dated future / option instruments).

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_deribit_archive.py
"""
from __future__ import annotations

import gzip
import io
import json
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

import flox_py  # noqa: E402

from flox_py.archives import ArchiveReader, deribit  # noqa: E402


# Deribit columns: trade_id, timestamp_ms, instrument, side, price,
# amount, mark_price, iv, index_price.
_ROWS_PERP = [
    (100, 1_700_000_000_500, "BTC-PERPETUAL", "buy",  42_100.5, 0.1, 42_100.0, 0.0, 42_098.0),
    (101, 1_700_000_001_250, "BTC-PERPETUAL", "sell", 42_101.0, 0.2, 42_101.0, 0.0, 42_099.5),
    (102, 1_700_000_002_000, "BTC-PERPETUAL", "buy",  42_100.7, 0.3, 42_100.7, 0.0, 42_098.5),
]

_ROWS_OPTION = [
    (200, 1_700_000_000_000, "BTC-29MAR24-50000-C", "buy",  0.0500, 10.0, 0.0498, 0.55, 42_000.0),
    (201, 1_700_000_001_000, "BTC-29MAR24-50000-C", "sell", 0.0510, 5.0,  0.0511, 0.56, 42_010.0),
]


def _csv_text(rows) -> str:
    buf = io.StringIO()
    buf.write("trade_id,timestamp_ms,instrument,side,price,amount,"
              "mark_price,iv,index_price\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    return buf.getvalue()


def _build_gz(dest: Path, rows) -> Path:
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(_csv_text(rows))
    return dest


class DeribitPerpetualTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="deribit-"))
        self.tape = self.tmp / "tape"

    def _convert(self) -> deribit.ConvertStats:
        g = self.tmp / "BTC-PERPETUAL-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_PERP)
        return deribit.trades_to_floxlog(
            g, self.tape,
            symbol_id=7, symbol_name="BTC-PERPETUAL", market="perpetual",
        )

    def test_round_trip(self) -> None:
        stats = self._convert()
        self.assertEqual(stats.trades_written, 3)
        trades = flox_py.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 3)
        sides = [int(t["side"]) for t in trades]
        self.assertEqual(sides, [0, 1, 0])

    def test_dedup_on_rerun(self) -> None:
        first = self._convert()
        self.assertEqual(first.trades_written, 3)
        second = deribit.trades_to_floxlog(
            self.tmp / "BTC-PERPETUAL-2024-01-15.csv.gz", self.tape,
            symbol_id=7, symbol_name="BTC-PERPETUAL", market="perpetual",
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)

    def test_metadata(self) -> None:
        self._convert()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "deribit")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTC-PERPETUAL")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USD")


class DeribitOptionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="deribit-opt-"))
        self.tape = self.tmp / "tape"

    def test_option_metadata(self) -> None:
        g = self.tmp / "BTC-29MAR24-50000-C-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_OPTION)
        stats = deribit.trades_to_floxlog(
            g, self.tape, symbol_id=7,
            symbol_name="BTC-29MAR24-50000-C", market="option",
        )
        self.assertEqual(stats.trades_written, 2)
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["instrument_type"], "option")
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTC-29MAR24-50000-C")
        self.assertEqual(sym["base_asset"], "BTC")

    def test_option_quotes_preserved(self) -> None:
        # mark/iv/index from the option CSV are kept as OptionQuote frames
        # instead of dropped, and read back via read_option_quotes_from.
        g = self.tmp / "BTC-29MAR24-50000-C-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_OPTION)
        deribit.trades_to_floxlog(
            g, self.tape, symbol_id=7,
            symbol_name="BTC-29MAR24-50000-C", market="option",
        )
        reader = flox_py.DataReader(str(self.tape))
        quotes = reader.read_option_quotes_from(0)
        self.assertEqual(quotes.size, 2)
        # Source row 200: mark=0.0498, iv=0.55, index=42000.0
        self.assertAlmostEqual(quotes["mark_price_raw"][0] / 1e8, 0.0498, places=6)
        self.assertAlmostEqual(quotes["iv_raw"][0] / 1e8, 0.55, places=6)
        self.assertAlmostEqual(quotes["index_price_raw"][0] / 1e8, 42_000.0, places=2)
        self.assertEqual(int(quotes["symbol_id"][0]), 7)
        # Trades are still written alongside the quotes.
        self.assertEqual(reader.read_trades().size, 2)

    def test_perpetual_emits_no_option_quotes(self) -> None:
        g = self.tmp / "BTC-PERPETUAL-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_PERP)
        deribit.trades_to_floxlog(
            g, self.tape, symbol_id=7, symbol_name="BTC-PERPETUAL", market="perpetual",
        )
        reader = flox_py.DataReader(str(self.tape))
        self.assertEqual(reader.read_option_quotes_from(0).size, 0)

    def test_protocol_compliance(self) -> None:
        self.assertIsInstance(deribit, ArchiveReader)


if __name__ == "__main__":
    unittest.main()
