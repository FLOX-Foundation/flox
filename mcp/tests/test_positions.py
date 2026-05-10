"""Tests for the live state inspection MCP tools (W2-T015 Phase 1)."""
from __future__ import annotations

import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from flox_mcp.tools import positions


def _sample_state(captured_at_ns: int = 1_700_000_000_000_000_000) -> dict:
    return {
        "schema_version": 1,
        "captured_at_ns": captured_at_ns,
        "kill_switch": {
            "active": False,
            "reason": None,
            "since_ns": None,
        },
        "strategies": [
            {"name": "ema-trend", "status": "running", "symbols": [1]},
            {"name": "kijun", "status": "paused", "symbols": [2]},
        ],
        "positions": [
            {
                "account": "bybit-prod",
                "strategy": "ema-trend",
                "symbol_id": 1,
                "symbol_name": "BTCUSDT",
                "qty": 0.5,
                "avg_price": 67432.10,
                "unrealized_pnl": 124.50,
            },
            {
                "account": "bitget-paper",
                "strategy": "kijun",
                "symbol_id": 2,
                "symbol_name": "ETHUSDT",
                "qty": -2.0,
                "avg_price": 3450.00,
                "unrealized_pnl": -32.00,
            },
        ],
        "open_orders": [
            {
                "order_id": "abc123",
                "account": "bybit-prod",
                "strategy": "ema-trend",
                "symbol_id": 1,
                "symbol_name": "BTCUSDT",
                "side": "BUY",
                "type": "LIMIT",
                "qty": 0.1,
                "price": 67000.0,
                "submitted_at_ns": 1_700_000_000_000_000_000,
            },
        ],
        "pnl": {
            "by_strategy": [
                {"strategy": "ema-trend", "realized": 1234.56,
                 "unrealized": 124.50, "fees": -12.34, "trades": 42},
                {"strategy": "kijun", "realized": -50.00,
                 "unrealized": -32.00, "fees": -3.50, "trades": 8},
            ],
            "total": {
                "realized": 1184.56,
                "unrealized": 92.50,
                "fees": -15.84,
            },
        },
    }


class _SnapshotTestCase(unittest.TestCase):
    """Common scaffolding: write a snapshot to a tmpfile, route the
    tools at it via the ``state_path`` arg."""

    def setUp(self) -> None:
        fd, path = tempfile.mkstemp(prefix="flox-state-", suffix=".json")
        os.close(fd)
        self.path = path
        self._write(_sample_state())

    def tearDown(self) -> None:
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass

    def _write(self, state: dict) -> None:
        with open(self.path, "w") as fh:
            json.dump(state, fh)


class GetPositionsTests(_SnapshotTestCase):
    def test_no_filter_returns_all(self) -> None:
        out = json.loads(positions.get_positions(state_path=self.path))
        self.assertEqual(len(out["data"]), 2)
        self.assertIsNotNone(out["snapshot_age_ms"])

    def test_filter_by_account(self) -> None:
        out = json.loads(positions.get_positions(
            account="bybit-prod", state_path=self.path))
        self.assertEqual(len(out["data"]), 1)
        self.assertEqual(out["data"][0]["symbol_name"], "BTCUSDT")

    def test_filter_by_strategy(self) -> None:
        out = json.loads(positions.get_positions(
            strategy="kijun", state_path=self.path))
        self.assertEqual(len(out["data"]), 1)
        self.assertEqual(out["data"][0]["account"], "bitget-paper")

    def test_filter_by_both_anded(self) -> None:
        out = json.loads(positions.get_positions(
            account="bybit-prod", strategy="kijun", state_path=self.path))
        self.assertEqual(out["data"], [])

    def test_missing_snapshot_returns_idle_response(self) -> None:
        """No snapshot is a normal state — return an
        engine_not_running marker, not an error. This is what makes
        the state_daemon.py workaround unnecessary."""
        out = json.loads(positions.get_positions(
            state_path="/no/such/file.json"))
        self.assertEqual(out["engine"], "not_running")
        self.assertEqual(out["data"], [])
        self.assertNotIn("error", out)
        self.assertIn("flox engine sim", out["hint"])

    def test_empty_snapshot_file_returns_idle_response(self) -> None:
        """An empty file is the same shape of 'engine hasn't written
        yet' as a missing file."""
        with tempfile.NamedTemporaryFile(
                "w", suffix=".json", delete=False) as f:
            empty = f.name
        try:
            out = json.loads(positions.get_positions(state_path=empty))
            self.assertEqual(out["engine"], "not_running")
            self.assertEqual(out["data"], [])
        finally:
            os.unlink(empty)

    def test_unsupported_schema_version_returns_error(self) -> None:
        """Corrupt-snapshot signals must keep failing loud — masking
        them would hide engine corruption."""
        bad = _sample_state()
        bad["schema_version"] = 999
        self._write(bad)
        out = json.loads(positions.get_positions(state_path=self.path))
        self.assertIn("error", out)
        self.assertIn("schema_version", out["error"])
        # Definitely not the idle path.
        self.assertNotIn("engine", out)


class GetOpenOrdersTests(_SnapshotTestCase):
    def test_no_filter_returns_all(self) -> None:
        out = json.loads(positions.get_open_orders(state_path=self.path))
        self.assertEqual(len(out["data"]), 1)

    def test_missing_snapshot_returns_idle(self) -> None:
        out = json.loads(positions.get_open_orders(
            state_path="/no/such/file.json"))
        self.assertEqual(out["engine"], "not_running")
        self.assertEqual(out["data"], [])

    def test_filter_substring_on_symbol(self) -> None:
        out = json.loads(positions.get_open_orders(
            filter="btc", state_path=self.path))
        self.assertEqual(len(out["data"]), 1)
        out = json.loads(positions.get_open_orders(
            filter="eth", state_path=self.path))
        self.assertEqual(out["data"], [])

    def test_filter_substring_on_strategy(self) -> None:
        out = json.loads(positions.get_open_orders(
            filter="ema", state_path=self.path))
        self.assertEqual(len(out["data"]), 1)


class GetPnlTests(_SnapshotTestCase):
    def test_total_returned_unfiltered(self) -> None:
        out = json.loads(positions.get_pnl(state_path=self.path))
        self.assertEqual(out["data"]["total"]["realized"], 1184.56)
        self.assertEqual(len(out["data"]["by_strategy"]), 2)

    def test_missing_snapshot_returns_idle(self) -> None:
        out = json.loads(positions.get_pnl(state_path="/no/such/file.json"))
        self.assertEqual(out["engine"], "not_running")
        self.assertEqual(out["data"], {"by_strategy": [], "total": {}})

    def test_strategy_filter_narrows_breakdown(self) -> None:
        out = json.loads(positions.get_pnl(
            strategy="ema-trend", state_path=self.path))
        self.assertEqual(len(out["data"]["by_strategy"]), 1)
        self.assertEqual(
            out["data"]["by_strategy"][0]["strategy"], "ema-trend")
        # total is still the full-snapshot total.
        self.assertEqual(out["data"]["total"]["realized"], 1184.56)


class GetKillSwitchTests(_SnapshotTestCase):
    def test_inactive_returns_active_false(self) -> None:
        out = json.loads(positions.get_kill_switch(state_path=self.path))
        self.assertFalse(out["data"]["active"])

    def test_missing_snapshot_returns_idle_inactive(self) -> None:
        """Defaulting to active=false when there is no engine is
        the right answer: there is no live trading to halt."""
        out = json.loads(positions.get_kill_switch(
            state_path="/no/such/file.json"))
        self.assertEqual(out["engine"], "not_running")
        self.assertFalse(out["data"]["active"])

    def test_active_returns_reason_and_since(self) -> None:
        active = _sample_state()
        active["kill_switch"] = {
            "active": True,
            "reason": "manual halt",
            "since_ns": 1_700_000_000_999_999_999,
        }
        self._write(active)
        out = json.loads(positions.get_kill_switch(state_path=self.path))
        self.assertTrue(out["data"]["active"])
        self.assertEqual(out["data"]["reason"], "manual halt")


class SnapshotAgeTests(_SnapshotTestCase):
    def test_recent_snapshot_age_is_small(self) -> None:
        recent = _sample_state(captured_at_ns=time.time_ns())
        self._write(recent)
        out = json.loads(positions.get_positions(state_path=self.path))
        self.assertIsNotNone(out["snapshot_age_ms"])
        self.assertLess(out["snapshot_age_ms"], 60_000)

    def test_missing_captured_at_ns_returns_none(self) -> None:
        no_ts = _sample_state()
        no_ts.pop("captured_at_ns")
        self._write(no_ts)
        out = json.loads(positions.get_positions(state_path=self.path))
        self.assertIsNone(out["snapshot_age_ms"])


class EnvVarPathTests(unittest.TestCase):
    """The FLOX_RUNTIME_STATE env var should be honored when no
    explicit state_path is passed."""

    def test_env_var_overrides_default(self) -> None:
        fd, path = tempfile.mkstemp(prefix="flox-state-env-", suffix=".json")
        os.close(fd)
        try:
            with open(path, "w") as fh:
                json.dump(_sample_state(), fh)
            old = os.environ.get("FLOX_RUNTIME_STATE")
            os.environ["FLOX_RUNTIME_STATE"] = path
            try:
                out = json.loads(positions.get_positions())
                self.assertEqual(len(out["data"]), 2)
            finally:
                if old is None:
                    os.environ.pop("FLOX_RUNTIME_STATE", None)
                else:
                    os.environ["FLOX_RUNTIME_STATE"] = old
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
