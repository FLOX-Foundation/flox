"""Tests for ``flox engine sim`` — bootstrap a paper-trading engine
in one command.

Smoke-level: argparse wires, strategy loader rejects bad inputs,
state writer produces a schema-v1 snapshot. End-to-end run against
a tape is skipped (it would block on Ctrl-C in v1) — the unit
pieces are covered individually.
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import cli, engine_cli  # noqa: E402


# ── Argparse wiring ────────────────────────────────────────────────


class EngineCliParserTests(unittest.TestCase):
    def test_engine_subcommand_registered(self) -> None:
        parser = cli._build_parser()
        out = io.StringIO()
        with redirect_stdout(out), self.assertRaises(SystemExit):
            parser.parse_args(["engine", "--help"])
        self.assertIn("sim", out.getvalue())

    def test_engine_sim_help(self) -> None:
        parser = cli._build_parser()
        out = io.StringIO()
        with redirect_stdout(out), self.assertRaises(SystemExit):
            parser.parse_args(["engine", "sim", "--help"])
        text = out.getvalue()
        self.assertIn("--strategy", text)
        self.assertIn("--tape", text)
        self.assertIn("--port", text)
        self.assertIn("--token", text)


# ── Strategy loader ────────────────────────────────────────────────


class StrategyLoaderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="flox-engine-cli-"))

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_missing_file_raises_systemexit(self) -> None:
        with self.assertRaises(SystemExit):
            engine_cli._load_strategy_class(self.tmp / "missing.py")

    def test_no_strategy_subclass_raises(self) -> None:
        path = self.tmp / "empty.py"
        path.write_text("# nothing here\n")
        with self.assertRaises(SystemExit) as ctx:
            engine_cli._load_strategy_class(path)
        self.assertIn("flox.Strategy subclass", str(ctx.exception))

    def test_multiple_subclasses_rejected(self) -> None:
        path = self.tmp / "two.py"
        path.write_text(
            "import flox_py\n"
            "class A(flox_py.Strategy): pass\n"
            "class B(flox_py.Strategy): pass\n"
        )
        with self.assertRaises(SystemExit) as ctx:
            engine_cli._load_strategy_class(path)
        self.assertIn("multiple", str(ctx.exception))

    def test_single_subclass_loads(self) -> None:
        path = self.tmp / "ok.py"
        path.write_text(
            "import flox_py\n"
            "class MyStrat(flox_py.Strategy):\n"
            "    pass\n"
        )
        cls = engine_cli._load_strategy_class(path)
        self.assertEqual(cls.__name__, "MyStrat")


# ── Kill switch stub ───────────────────────────────────────────────


class KillSwitchStubTests(unittest.TestCase):
    def test_idle_state(self) -> None:
        ks = engine_cli._KillSwitch()
        s = ks.state()
        self.assertFalse(s["active"])
        self.assertIsNone(s["reason"])
        self.assertIsNone(s["since_ns"])

    def test_set_active_records_reason_and_timestamp(self) -> None:
        ks = engine_cli._KillSwitch()
        ks.set(True, "manual halt")
        s = ks.state()
        self.assertTrue(s["active"])
        self.assertEqual(s["reason"], "manual halt")
        self.assertIsNotNone(s["since_ns"])

    def test_clear_drops_reason(self) -> None:
        ks = engine_cli._KillSwitch()
        ks.set(True, "panic")
        ks.set(False)
        s = ks.state()
        self.assertFalse(s["active"])
        self.assertIsNone(s["reason"])


# ── State writer ───────────────────────────────────────────────────


class StateWriterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="flox-state-writer-"))
        self.path = self.tmp / "runtime.json"

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_writes_schema_v1_snapshot(self) -> None:
        ks = engine_cli._KillSwitch()
        w = engine_cli._StateWriter(
            path=self.path, kill_switch=ks,
            strategy_name="MyStrat",
            symbol_id=1, symbol_name="BTCUSDT",
            period_s=10.0,  # large; we'll only fire the immediate write
        )
        w.start()
        try:
            self.assertTrue(self.path.exists())
            data = json.loads(self.path.read_text())
            self.assertEqual(data["schema_version"], 1)
            self.assertEqual(len(data["strategies"]), 1)
            self.assertEqual(data["strategies"][0]["name"], "MyStrat")
            self.assertFalse(data["kill_switch"]["active"])
            # Schema for tier-4 read tools.
            self.assertIn("positions", data)
            self.assertIn("open_orders", data)
            self.assertIn("pnl", data)
        finally:
            w.stop()

    def test_kill_switch_state_propagates(self) -> None:
        ks = engine_cli._KillSwitch()
        w = engine_cli._StateWriter(
            path=self.path, kill_switch=ks,
            strategy_name="S", symbol_id=1, symbol_name="X",
            period_s=10.0,
        )
        w.start()
        try:
            # Trip the kill-switch and force a re-write.
            ks.set(True, "test halt")
            w._write_once()
            data = json.loads(self.path.read_text())
            self.assertTrue(data["kill_switch"]["active"])
            self.assertEqual(data["kill_switch"]["reason"], "test halt")
        finally:
            w.stop()


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
