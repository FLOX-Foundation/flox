"""Tests for ``flox_py.archives.okx`` — OKX public archive converter.

No network: synthetic CSV matching OKX's published column layout is
built in memory (as both .zip and .csv.gz variants) and fed through
the converter. The resulting `.floxlog` is read back via DataReader
and asserted trade-by-trade.

Covers:

  * `trades_to_floxlog` round-trip + side mapping (buy → 0, sell → 1).
  * `.zip` (with one CSV inside) and `.csv.gz` packaging both parse.
  * Dedup on re-run via integer `trade_id`.
  * `metadata.json` carries `exchange=okx`, instrument_type per market.
  * Protocol compliance: `okx` satisfies `ArchiveReader`.
  * Cross-exchange merge: binance:BTCUSDT + okx:BTC-USDT-SWAP keep
    distinct (exchange, name) keys in MergedTapeReader.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_okx_archive.py
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

from flox_py.archives import ArchiveReader, okx  # noqa: E402


# OKX columns: trade_id, side, size, price, timestamp_ms.
_ROWS = [
    (100, "buy",  0.01, 42_100.5, 1_700_000_000_500),
    (101, "sell", 0.02, 42_101.0, 1_700_000_001_250),
    (102, "buy",  0.03, 42_100.7, 1_700_000_002_000),
]


def _csv_text(rows=_ROWS) -> str:
    buf = io.StringIO()
    buf.write("trade_id,side,size,price,timestamp_ms\n")
    for r in rows:
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


class OkxConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="okx-"))
        self.tape = self.tmp / "tape"

    def _convert_zip(self) -> okx.ConvertStats:
        z = self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip"
        _build_zip(z)
        return okx.trades_to_floxlog(
            z, self.tape,
            symbol_id=7, symbol_name="BTC-USDT-SWAP", market="swap",
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
        g = self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.csv.gz"
        _build_gz(g)
        stats = okx.trades_to_floxlog(
            g, self.tape,
            symbol_id=7, symbol_name="BTC-USDT-SWAP", market="swap",
        )
        self.assertEqual(stats.trades_written, 3)

    def test_dedup_on_rerun(self) -> None:
        first = self._convert_zip()
        self.assertEqual(first.trades_written, 3)
        second = okx.trades_to_floxlog(
            self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip", self.tape,
            symbol_id=7, symbol_name="BTC-USDT-SWAP", market="swap",
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)

    def test_metadata(self) -> None:
        self._convert_zip()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "okx")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTC-USDT-SWAP")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_protocol_compliance(self) -> None:
        self.assertIsInstance(okx, ArchiveReader)

    def test_option_market_metadata(self) -> None:
        z = self.tmp / "BTC-29MAR24-50000-C-trades-2024-01-15.zip"
        _build_zip(z)
        okx.trades_to_floxlog(
            z, self.tape, symbol_id=7,
            symbol_name="BTC-29MAR24-50000-C", market="option",
        )
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["instrument_type"], "option")
        sym = meta["symbols"][0]
        # The base/quote helper picks the first two hyphen-separated
        # tokens; option-chain naming uses BTC + expiry as those two.
        self.assertEqual(sym["base_asset"], "BTC")


class CrossExchangeMergeTests(unittest.TestCase):
    def setUp(self) -> None:
        from flox_py.archives import binance as bnc

        self.tmp = Path(tempfile.mkdtemp(prefix="okx-xmrg-"))
        bnz = self.tmp / "BTCUSDT-aggTrades-2024-01-15.zip"
        buf = io.StringIO()
        for i in range(2):
            buf.write(
                f"{1000+i},42000.0,0.01,{i},{i},"
                f"{1_700_000_000_500 + i * 1000},True,True\n"
            )
        with zipfile.ZipFile(bnz, "w") as z:
            z.writestr(bnz.with_suffix(".csv").name, buf.getvalue())
        self.bnz_tape = self.tmp / "binance-tape"
        bnc.aggtrades_to_floxlog(
            bnz, self.bnz_tape, symbol_id=1, symbol_name="BTCUSDT",
            market="um-futures",
        )
        z = self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip"
        _build_zip(z)
        self.okx_tape = self.tmp / "okx-tape"
        okx.trades_to_floxlog(
            z, self.okx_tape, symbol_id=1, symbol_name="BTC-USDT-SWAP",
            market="swap",
        )

    def test_merge(self) -> None:
        reader = flox_py.MergedTapeReader([
            str(self.bnz_tape), str(self.okx_tape),
        ])
        trades = reader.read_trades()
        self.assertEqual(int(trades.size), 5)
        sym_table = reader.symbol_table()
        exchanges = sorted({s["exchange"] for s in sym_table})
        self.assertEqual(exchanges, ["binance", "okx"])
        names = sorted({s["name"] for s in sym_table})
        # Two distinct names; the two tapes do not collide.
        self.assertEqual(names, ["BTC-USDT-SWAP", "BTCUSDT"])


if __name__ == "__main__":
    unittest.main()
