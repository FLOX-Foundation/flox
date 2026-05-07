"""Tests for the ``flox tape`` CLI subcommands.

Covers the recorder hook ↔ DataWriter wiring, replay round-trip, and
the inspect command. The recorder is exercised end-to-end without
ccxt — we feed synthetic trades through a Runner with the recorder
attached, then verify the resulting ``.floxlog`` directory is
readable via DataReader and contains the same trades in order.

The CLI itself (`flox tape record`, `flox tape replay`,
`flox tape inspect`) is tested at the argparse level — wiring
exists, argument parsing works, error paths fire cleanly.
"""
from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import flox_py as flox  # noqa: E402

from flox_py import cli, tape  # noqa: E402


# ── Recorder hook end-to-end ───────────────────────────────────────


class RecorderRoundTripTests(unittest.TestCase):
    """Pump trades through a Runner with the tape recorder attached;
    confirm the on-disk format reads back identical."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="flox-tape-rec-"))

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_recorder_writes_trades_then_reader_reads_them_back(self) -> None:
        registry = flox.SymbolRegistry()
        sym = registry.add_symbol("test", "BTCUSDT", tick_size=0.01)

        recorder = tape.make_recorder_hook(self.tmp, max_segment_mb=64)
        runner = flox.Runner(registry, on_signal=lambda _sig: None)
        runner.set_market_data_recorder(recorder)
        runner.start()

        # Three synthetic trades with monotonically increasing timestamps.
        synthetic = [
            (101.50, 0.5, True, 1_000_000_000),
            (101.55, 0.3, False, 1_500_000_000),
            (101.60, 1.2, True, 2_000_000_000),
        ]
        for price, qty, is_buy, ts_ns in synthetic:
            runner.on_trade(sym, price, qty, is_buy, ts_ns)

        runner.stop()
        recorder.close()

        stats = tape.inspect_tape(self.tmp)
        self.assertEqual(stats.trade_count, 3)
        self.assertEqual(stats.first_ts_ns, 1_000_000_000)
        self.assertEqual(stats.last_ts_ns, 2_000_000_000)

        # Replay round-trip reproduces the exact trades.
        seen = []
        n = tape.replay_tape(self.tmp, on_trade=lambda ts, s, p, q, side: seen.append((p, q, side, ts)))
        self.assertEqual(n, 3)
        self.assertEqual([t[0] for t in seen], [101.50, 101.55, 101.60])
        self.assertEqual([t[3] for t in seen],
                          [1_000_000_000, 1_500_000_000, 2_000_000_000])
        # side: 0=buy, 1=sell — matches our (True, False, True) input.
        self.assertEqual([t[2] for t in seen], [0, 1, 0])

    def test_recorder_book_updates_are_counted_not_written(self) -> None:
        # v1 only writes trades. Book updates should count toward
        # `book_updates_skipped` so the CLI can surface "you tried to
        # record book and got nothing" instead of silent loss.
        registry = flox.SymbolRegistry()
        sym = registry.add_symbol("test", "BTCUSDT", tick_size=0.01)
        recorder = tape.make_recorder_hook(self.tmp)
        runner = flox.Runner(registry, on_signal=lambda _sig: None)
        runner.set_market_data_recorder(recorder)
        runner.start()
        runner.on_book_snapshot(sym, [100.0], [1.0], [101.0], [1.0], 1_000_000)
        runner.stop()
        recorder.close()
        self.assertEqual(recorder.stats.book_updates_skipped, 1)
        self.assertEqual(recorder.stats.trades_written, 0)

    def test_inspect_on_empty_directory(self) -> None:
        empty_dir = self.tmp / "empty"
        empty_dir.mkdir()
        # Need at least an empty DataWriter so DataReader doesn't blow up.
        w = flox.DataWriter(str(empty_dir))
        w.close()
        stats = tape.inspect_tape(empty_dir)
        self.assertEqual(stats.trade_count, 0)


# ── CLI surface ────────────────────────────────────────────────────


class CliTapeTests(unittest.TestCase):
    """Argparse + command dispatch checks; no live ccxt or async run."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="flox-tape-cli-"))

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_parser_registers_tape_subcommand(self) -> None:
        # Smoke-test: argparse builds, `flox tape --help` includes the
        # three subcommands.
        parser = cli._build_parser()
        out = io.StringIO()
        with redirect_stdout(out), self.assertRaises(SystemExit):
            parser.parse_args(["tape", "--help"])
        text = out.getvalue()
        self.assertIn("record", text)
        self.assertIn("replay", text)
        self.assertIn("inspect", text)

    def test_inspect_missing_path_errors(self) -> None:
        err = io.StringIO()
        with redirect_stderr(err):
            rc = cli.main(["tape", "inspect", str(self.tmp / "nope")])
        self.assertNotEqual(rc, 0)
        self.assertIn("path not found", err.getvalue())

    def test_replay_missing_path_errors(self) -> None:
        err = io.StringIO()
        with redirect_stderr(err):
            rc = cli.main(["tape", "replay", str(self.tmp / "nope")])
        self.assertNotEqual(rc, 0)
        self.assertIn("path not found", err.getvalue())

    def test_inspect_round_trip_via_cli(self) -> None:
        # Build a tape via the recorder hook, then inspect via CLI.
        registry = flox.SymbolRegistry()
        sym = registry.add_symbol("test", "BTC", tick_size=0.01)
        rec = tape.make_recorder_hook(self.tmp)
        r = flox.Runner(registry, on_signal=lambda _s: None)
        r.set_market_data_recorder(rec)
        r.start()
        r.on_trade(sym, 100.0, 1.0, True, 1_000_000)
        r.on_trade(sym, 101.0, 2.0, False, 2_000_000)
        r.stop()
        rec.close()

        out = io.StringIO()
        with redirect_stdout(out):
            rc = cli.main(["tape", "inspect", str(self.tmp)])
        self.assertEqual(rc, 0)
        text = out.getvalue()
        self.assertIn("trades       : 2", text)
        self.assertIn("symbols", text)

    def test_duration_parser(self) -> None:
        self.assertEqual(cli._parse_duration("60s"), 60.0)
        self.assertEqual(cli._parse_duration("5m"), 300.0)
        self.assertEqual(cli._parse_duration("1h"), 3600.0)
        self.assertEqual(cli._parse_duration("2d"), 172800.0)
        self.assertEqual(cli._parse_duration("90"), 90.0)
        with self.assertRaises(ValueError):
            cli._parse_duration("")

    def test_normalize_ccxt_symbol(self) -> None:
        self.assertEqual(cli._normalize_ccxt_symbol("BTC/USDT"), "BTC/USDT")
        self.assertEqual(cli._normalize_ccxt_symbol("BTCUSDT"), "BTC/USDT")
        self.assertEqual(cli._normalize_ccxt_symbol("ETHUSDC"), "ETH/USDC")
        self.assertEqual(cli._normalize_ccxt_symbol("SOLBTC"), "SOL/BTC")
        # Unknown quote → pass through unchanged so the user can fix.
        self.assertEqual(cli._normalize_ccxt_symbol("FOOBAR"), "FOOBAR")


if __name__ == "__main__":
    unittest.main()
