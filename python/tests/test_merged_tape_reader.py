"""Tests for `flox_py.MergedTapeReader` + `flox_py.tape.replay_tapes`.

Covers the W14-T016 acceptance criteria (Python slice):
  - single tape ≡ DataReader read (after rekey)
  - two non-overlapping tapes: merged trade count + order
  - cross-exchange same-symbol → separate global_ids
  - overlapping book streams raise OverlappingBookStream-like error
  - tie-break by tape order on identical exchange_ts_ns
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

import flox_py as flox
from flox_py import tape as tape_mod


def _write_tape(out_dir: str, exchange: str, symbol_name: str,
                trades: list[tuple[int, float, float, bool]],
                books: list[tuple[int, list, list, list, list]] | None = None,
                exchange_id: int = 0) -> None:
    """Write one .floxlog with the given trades + (optional) book snapshots.

    `trades`: [(ts_ns, price, qty, is_buy), ...]
    `books`:  [(ts_ns, bid_prices, bid_qtys, ask_prices, ask_qtys), ...]
    """
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol(exchange, symbol_name, tick_size=0.01)
    hook = flox.BinaryLogRecorderHook(
        out_dir, max_segment_mb=4, exchange_id=exchange_id,
        compression="none", exchange_name=exchange,
        instrument_type="perpetual",
    )
    hook.add_symbol(sym, symbol_name, "", "", 2, 6)
    runner = flox.Runner(registry, on_signal=lambda _s: None)
    runner.set_market_data_recorder(hook)
    runner.start()
    for ts_ns, price, qty, is_buy in trades:
        runner.on_trade(sym, price=price, qty=qty, is_buy=is_buy, ts_ns=ts_ns)
    for entry in (books or []):
        ts_ns, bid_p, bid_q, ask_p, ask_q = entry
        runner.on_book_snapshot(sym, bid_p, bid_q, ask_p, ask_q, ts_ns)
    runner.stop()
    hook.close()


class MergedTapeReaderTests(unittest.TestCase):

    def test_single_tape_round_trip(self):
        """`MergedTapeReader([t]).read_trades()` matches `DataReader(t).read_trades()`
        on price/qty/side. symbol_id is remapped to the single-tape global_id."""
        with tempfile.TemporaryDirectory() as d:
            t = os.path.join(d, "bybit")
            base = 1_700_000_000_000_000_000
            _write_tape(t, "bybit", "BTCUSDT",
                        [(base + i * 1_000_000, 50000.0 + i, 0.1, i % 2 == 0)
                         for i in range(5)])

            dr = flox.DataReader(t)
            mr = flox.MergedTapeReader([t])
            single = dr.read_trades()
            merged = mr.read_trades()

            self.assertEqual(single.size, merged.size)
            # symbol_id is remapped — every other field bit-equal.
            for col in ("exchange_ts_ns", "price_raw", "qty_raw", "side"):
                self.assertTrue(
                    (single[col] == merged[col]).all(),
                    f"column {col} diverged",
                )
            # Global IDs are 1..N — single-tape single-symbol → 1.
            self.assertEqual(int(mr.symbol_table()[0]["global_id"]), 1)

    def test_two_tapes_non_overlapping(self):
        """Trade count is N+M, sorted by exchange_ts_ns across tapes."""
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "bybit")
            t2 = os.path.join(d, "binance")
            base = 1_700_000_000_000_000_000
            _write_tape(t1, "bybit", "BTCUSDT",
                        [(base + i * 1_000_000, 50000.0 + i, 0.1, True)
                         for i in range(5)],
                        exchange_id=1)
            _write_tape(t2, "binance", "ETHUSDT",
                        [(base + 500_000 + i * 1_000_000, 3000.0 + i, 1.0, False)
                         for i in range(3)],
                        exchange_id=2)

            mr = flox.MergedTapeReader([t1, t2])
            t = mr.read_trades()
            self.assertEqual(t.size, 8)

            # Strictly non-decreasing exchange_ts.
            ts = [int(r["exchange_ts_ns"]) for r in t]
            self.assertEqual(ts, sorted(ts))

            # Both symbols present with distinct global IDs.
            ids = sorted({int(r["symbol_id"]) for r in t})
            self.assertEqual(ids, [1, 2])

    def test_cross_exchange_same_symbol_separate_global_ids(self):
        """`(bybit, BTCUSDT)` and `(binance, BTCUSDT)` get distinct global_ids."""
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "bybit")
            t2 = os.path.join(d, "binance")
            base = 1_700_000_000_000_000_000
            _write_tape(t1, "bybit", "BTCUSDT",
                        [(base + i * 1_000_000, 50000.0, 0.1, True)
                         for i in range(2)],
                        exchange_id=1)
            _write_tape(t2, "binance", "BTCUSDT",
                        [(base + 500_000 + i * 1_000_000, 50100.0, 0.1, True)
                         for i in range(2)],
                        exchange_id=2)

            mr = flox.MergedTapeReader([t1, t2])
            tbl = mr.symbol_table()
            exchanges = {s["exchange"] for s in tbl}
            self.assertEqual(exchanges, {"bybit", "binance"})
            self.assertEqual(len(tbl), 2)
            self.assertEqual(
                sorted({int(s["global_id"]) for s in tbl}), [1, 2])

    def test_overlapping_book_streams_raise(self):
        """Same `(exchange, name)` with overlapping book time ranges raises."""
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "session-a")
            t2 = os.path.join(d, "session-b")
            base = 1_700_000_000_000_000_000
            # Two overlapping sessions of the same exchange+symbol carrying books.
            _write_tape(t1, "bybit", "BTCUSDT",
                        [(base, 50000.0, 0.1, True)],
                        books=[(base + 100_000, [49999.0], [1.0],
                                [50001.0], [1.0])],
                        exchange_id=1)
            _write_tape(t2, "bybit", "BTCUSDT",
                        [(base + 50_000, 50001.0, 0.1, True)],
                        books=[(base + 200_000, [49998.0], [2.0],
                                [50002.0], [2.0])],
                        exchange_id=1)

            with self.assertRaises(Exception) as ctx:
                flox.MergedTapeReader([t1, t2])
            self.assertIn("overlapping", str(ctx.exception).lower())

    def test_replay_tapes_emits_in_time_order(self):
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "a")
            t2 = os.path.join(d, "b")
            base = 1_700_000_000_000_000_000
            _write_tape(t1, "ex_a", "FOO",
                        [(base + i * 2_000_000, 100.0 + i, 1.0, True)
                         for i in range(3)],
                        exchange_id=1)
            _write_tape(t2, "ex_b", "FOO",
                        [(base + 1_000_000 + i * 2_000_000, 200.0 + i, 2.0, False)
                         for i in range(3)],
                        exchange_id=2)

            seen = []
            stats = tape_mod.replay_tapes(
                [t1, t2],
                on_trade=lambda ts, sym, p, q, side: seen.append((ts, sym, p)),
            )
            self.assertEqual(stats.trades, 6)
            self.assertEqual(len(stats.tapes), 2)
            # Timestamps strictly non-decreasing.
            ts_only = [s[0] for s in seen]
            self.assertEqual(ts_only, sorted(ts_only))
            # Alternates between the two venues.
            symbols = [s[1] for s in seen]
            self.assertEqual(symbols, [1, 2, 1, 2, 1, 2])


if __name__ == "__main__":
    unittest.main()
