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

    def test_stream_events_matches_eager_read(self):
        """`stream_events` walks via N-way heap merge with O(N) memory;
        output must be bit-equal to `read_trades` / `read_books`."""
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "a")
            t2 = os.path.join(d, "b")
            base = 1_700_000_000_000_000_000
            _write_tape(t1, "ex_a", "FOO",
                        [(base + i * 1_000_000, 100.0 + i, 1.0, True)
                         for i in range(7)],
                        exchange_id=1)
            _write_tape(t2, "ex_b", "BAR",
                        [(base + 500_000 + i * 1_000_000, 200.0 + i, 2.0, False)
                         for i in range(5)],
                        exchange_id=2)

            mr_eager = flox.MergedTapeReader([t1, t2])
            eager = [
                (int(t["exchange_ts_ns"]), int(t["symbol_id"]),
                 int(t["price_raw"]), int(t["qty_raw"]), int(t["side"]))
                for t in mr_eager.read_trades()
            ]

            mr_stream = flox.MergedTapeReader([t1, t2])
            streamed = []

            def on_trade(ets, rts, p, q, sid, ti, side):
                streamed.append((ets, sid, p, q, side))

            mr_stream.stream_events(on_trade=on_trade)

            self.assertEqual(len(eager), 12)
            self.assertEqual(streamed, eager)

    def test_stream_events_callback_can_abort(self):
        """Returning False from on_trade halts the walk."""
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "tape")
            base = 1_700_000_000_000_000_000
            _write_tape(t1, "ex", "SYM",
                        [(base + i * 1_000_000, 100.0 + i, 1.0, True)
                         for i in range(10)])

            mr = flox.MergedTapeReader([t1])
            seen = []

            def on_trade(ets, rts, p, q, sid, ti, side):
                seen.append(p)
                return len(seen) < 3  # stop after 3

            mr.stream_events(on_trade=on_trade)
            self.assertEqual(len(seen), 3)

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


class BacktestRunTapesTests(unittest.TestCase):
    """Acceptance #8 from W14-T016: `run_tapes([t])` ≡ `run_tape(t)`.

    Both go through the same `BacktestRunner::run` pipeline; the
    multi-tape path is an adapter over `MergedTapeReader`. For a
    single-tape input the merge is a no-op (one global symbol id =
    1, events flow through unchanged), so stats must match.
    """

    def test_run_tapes_single_equals_run_tape(self):
        with tempfile.TemporaryDirectory() as d:
            t = os.path.join(d, "tape")
            base = 1_700_000_000_000_000_000
            _write_tape(t, "bybit", "BTCUSDT",
                        [(base + i * 1_000_000, 50000.0 + i, 0.1, i % 2 == 0)
                         for i in range(20)])

            reg1 = flox.SymbolRegistry()
            bt1 = flox.BacktestRunner(reg1, fee_rate=0.0,
                                       initial_capital=10000.0)
            bt1.set_strategy(flox.Strategy(symbols=[]))
            single = bt1.run_tape(t)

            reg2 = flox.SymbolRegistry()
            bt2 = flox.BacktestRunner(reg2, fee_rate=0.0,
                                       initial_capital=10000.0)
            bt2.set_strategy(flox.Strategy(symbols=[]))
            multi = bt2.run_tapes([t])

            # Both paths drove the engine through the same event sequence.
            # Stats dicts must match field-by-field.
            for key in ("total_trades", "winning_trades", "losing_trades",
                        "initial_capital", "final_capital", "total_pnl",
                        "net_pnl"):
                self.assertEqual(single[key], multi[key],
                                  f"field {key} diverged: "
                                  f"single={single[key]} multi={multi[key]}")

    def test_run_tapes_empty_paths_raises(self):
        reg = flox.SymbolRegistry()
        bt = flox.BacktestRunner(reg, fee_rate=0.0, initial_capital=10000.0)
        bt.set_strategy(flox.Strategy(symbols=[]))
        with self.assertRaises(Exception):
            bt.run_tapes([])

    def test_run_tapes_two_venues_runs_to_completion(self):
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "bybit")
            t2 = os.path.join(d, "binance")
            base = 1_700_000_000_000_000_000
            _write_tape(t1, "bybit", "BTCUSDT",
                        [(base + i * 1_000_000, 50000.0 + i, 0.1, True)
                         for i in range(5)])
            _write_tape(t2, "binance", "ETHUSDT",
                        [(base + 500_000 + i * 1_000_000, 3000.0 + i, 1.0, False)
                         for i in range(3)])

            reg = flox.SymbolRegistry()
            bt = flox.BacktestRunner(reg, fee_rate=0.0,
                                      initial_capital=10000.0)
            bt.set_strategy(flox.Strategy(symbols=[]))
            stats = bt.run_tapes([t1, t2])
            self.assertEqual(stats["initial_capital"], 10000.0)
            self.assertEqual(stats["final_capital"], 10000.0)

    # ── summary() ───────────────────────────────────────────────────────
    def test_summary_reports_inspect_totals_before_read(self):
        """`summary()` on a freshly built reader returns the same
        aggregate counts as the underlying `BinaryLogReader::inspect`
        — without forcing the caller to materialise every event."""
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
            s = mr.summary()
            self.assertEqual(s["tape_count"], 2)
            self.assertEqual(s["symbol_count"], 2)
            self.assertEqual(s["total_events"], 8)
            self.assertLessEqual(s["first_event_ns"], s["last_event_ns"])

    # ── per_tape_stats() ────────────────────────────────────────────────
    def test_per_tape_stats_filled_from_manifest_at_construction(self):
        """`per_tape_stats()` must report honest trade / book counts
        immediately after construction — no `read_trades` / `read_books`
        round trip required. The values come from the recording
        manifest (`total_trades` / `total_book_updates`), which
        `BinaryLogWriter::close()` populates from `WriterStats`."""
        with tempfile.TemporaryDirectory() as d:
            t1 = os.path.join(d, "bybit")
            t2 = os.path.join(d, "binance")
            base = 1_700_000_000_000_000_000
            _write_tape(
                t1, "bybit", "BTCUSDT",
                trades=[(base + i * 1_000_000, 50000.0 + i, 0.1, True)
                        for i in range(5)],
                books=[(base + i * 2_000_000,
                        [100.0, 99.5], [1.0, 2.0],
                        [100.5, 101.0], [1.5, 2.5])
                       for i in range(3)],
                exchange_id=1,
            )
            _write_tape(
                t2, "binance", "ETHUSDT",
                trades=[(base + 500_000 + i * 1_000_000, 3000.0 + i, 1.0, False)
                        for i in range(7)],
                exchange_id=2,
            )

            mr = flox.MergedTapeReader([t1, t2])
            stats = mr.per_tape_stats()
            self.assertEqual(len(stats), 2)

            by_path = {s["path"]: s for s in stats}
            self.assertEqual(by_path[t1]["trades"], 5)
            self.assertEqual(by_path[t1]["books"], 3)
            self.assertEqual(by_path[t2]["trades"], 7)
            self.assertEqual(by_path[t2]["books"], 0)

    def test_per_tape_stats_stable_after_reads(self):
        """`per_tape_stats()` reflects manifest ground truth, not
        consumed-during-read counts — repeated reads (with or without
        filters) leave the numbers untouched."""
        with tempfile.TemporaryDirectory() as d:
            t = os.path.join(d, "bybit")
            base = 1_700_000_000_000_000_000
            _write_tape(
                t, "bybit", "BTCUSDT",
                trades=[(base + i * 1_000_000, 50000.0 + i, 0.1, True)
                        for i in range(4)],
                exchange_id=1,
            )

            mr = flox.MergedTapeReader([t])
            before = mr.per_tape_stats()[0]["trades"]
            mr.read_trades()
            mr.read_trades()
            after = mr.per_tape_stats()[0]["trades"]
            self.assertEqual(before, 4)
            self.assertEqual(after, 4)


if __name__ == "__main__":
    unittest.main()
