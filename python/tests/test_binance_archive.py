"""Tests for ``flox_py.archives.binance`` — Binance public aggTrades
zip → floxlog converter.

No network: a synthetic ``aggTrades.zip`` matching the exact column
layout published on ``data.binance.vision`` is built in-memory and
fed through the converter. The resulting `.floxlog` directory is
read back via ``flox_py.DataReader`` and asserted trade-by-trade.

Covers:

  * Single-day conversion via ``aggtrades_to_floxlog``.
  * Side mapping: ``is_buyer_maker=True`` → ``Side::SELL``.
  * Header-row autoskip (older daily files prepend a header).
  * Append + dedup: re-running the same day writes zero new trades.
  * ``metadata.json`` is produced with exchange / instrument_type /
    symbol entry / time range.
  * CLI wiring: ``flox archive binance --csv ... --out ...``.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_binance_archive.py
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import flox_py  # noqa: E402

from flox_py import cli  # noqa: E402
from flox_py.archives import binance as bnc  # noqa: E402


# Synthetic rows mirroring the public archive layout. The third column
# of each tuple is is_buyer_maker, which the converter must map to
# Side::SELL (1) when true and Side::BUY (0) when false.
_ROWS = [
    # (agg_id, price, qty, first_id, last_id, ts_ms, is_buyer_maker, is_best)
    (1001, 42100.50, 0.005, 9001, 9001, 1_700_000_000_000, "True",  "True"),
    (1002, 42101.00, 0.010, 9002, 9003, 1_700_000_001_000, "False", "True"),
    (1003, 42100.75, 0.002, 9004, 9004, 1_700_000_002_500, "True",  "True"),
    (1004, 42102.25, 0.020, 9005, 9006, 1_700_000_003_750, "False", "True"),
]


def _build_zip(dest: Path, *, with_header: bool, rows=_ROWS) -> Path:
    """Pack ``rows`` into a Binance-style aggTrades zip."""
    buf = io.StringIO()
    if with_header:
        buf.write("agg_trade_id,price,quantity,first_trade_id,last_trade_id,"
                  "transact_time,is_buyer_maker,is_best_match\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    csv_bytes = buf.getvalue().encode("ascii")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, csv_bytes)
    return dest


class SingleDayConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-archive-"))
        self.zip_path = self.tmp / "BTCUSDT-aggTrades-2024-01-15.zip"
        self.tape = self.tmp / "tape"

    def _convert(self, *, with_header: bool, append=True) -> bnc.ConvertStats:
        _build_zip(self.zip_path, with_header=with_header)
        return bnc.aggtrades_to_floxlog(
            self.zip_path,
            self.tape,
            symbol_id=7,
            symbol_name="BTCUSDT",
            market="um-futures",
            append=append,
        )

    def test_round_trip_with_header(self) -> None:
        stats = self._convert(with_header=True)
        self.assertEqual(stats.trades_written, 4)
        self.assertEqual(stats.rows_read, 4)
        self.assertEqual(stats.rows_skipped, 0)
        self._assert_tape_matches()

    def test_round_trip_no_header(self) -> None:
        stats = self._convert(with_header=False)
        self.assertEqual(stats.trades_written, 4)
        self._assert_tape_matches()

    def test_side_mapping(self) -> None:
        self._convert(with_header=False)
        trades = flox_py.DataReader(str(self.tape)).read_trades()
        # is_buyer_maker=True → SELL (1); False → BUY (0).
        sides = [int(t["side"]) for t in trades]
        self.assertEqual(sides, [1, 0, 1, 0])

    def test_append_is_dedup(self) -> None:
        first = self._convert(with_header=False)
        self.assertEqual(first.trades_written, 4)
        # Re-run the same day. Every row's agg_id is ≤ last_trade_id,
        # so nothing new lands in the tape.
        second = bnc.aggtrades_to_floxlog(
            self.zip_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="um-futures",
            append=True,
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 4)
        trades = flox_py.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 4)

    def test_metadata_emitted(self) -> None:
        self._convert(with_header=False)
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "binance")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 4)
        self.assertTrue(meta["has_trades"])
        self.assertFalse(meta["has_book_snapshots"])
        self.assertEqual(len(meta["symbols"]), 1)
        sym = meta["symbols"][0]
        self.assertEqual(sym["symbol_id"], 7)
        self.assertEqual(sym["name"], "BTCUSDT")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_cli_wiring(self) -> None:
        _build_zip(self.zip_path, with_header=False)
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = cli.main([
                "archive", "binance",
                "--csv", str(self.zip_path),
                "--out", str(self.tape),
                "--symbol-id", "7",
                "--symbol", "BTCUSDT",
                "--market", "um-futures",
            ])
        self.assertEqual(rc, 0, msg=buf.getvalue())
        out = buf.getvalue()
        self.assertIn("trades_written=4", out)
        trades = flox_py.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 4)

    def _assert_tape_matches(self) -> None:
        trades = flox_py.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 4)
        # Trade IDs round-trip as agg_trade_id.
        self.assertEqual([int(t["trade_id"]) for t in trades],
                         [1001, 1002, 1003, 1004])
        # Timestamps round-trip as ms → ns.
        self.assertEqual([int(t["exchange_ts_ns"]) for t in trades],
                         [r[5] * 1_000_000 for r in _ROWS])
        # Symbol IDs match the converter's argument.
        for t in trades:
            self.assertEqual(int(t["symbol_id"]), 7)
        # Prices round-trip within the 1e-8 fixed-point grid.
        for t, r in zip(trades, _ROWS):
            self.assertAlmostEqual(float(t["price_raw"]) / 1e8, r[1], places=6)
            self.assertAlmostEqual(float(t["qty_raw"]) / 1e8, r[2], places=6)


if __name__ == "__main__":
    unittest.main()
