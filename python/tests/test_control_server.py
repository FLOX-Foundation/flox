"""Tests for ``flox_py.control_server.ControlServer``."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import control_server  # noqa: E402


class _FakeExecutor:
    def __init__(self) -> None:
        self.submitted: List[Dict[str, Any]] = []
        self.cancelled: List[int] = []
        self.cancelled_all: List[int] = []

    def submit_order(self, id_, side, price, qty, type, symbol):  # noqa: A002
        self.submitted.append({
            "id": int(id_), "side": side, "price": float(price),
            "qty": float(qty), "type": type, "symbol": int(symbol),
        })

    def cancel_order(self, order_id):
        self.cancelled.append(int(order_id))

    def cancel_all(self, symbol):
        self.cancelled_all.append(int(symbol))


class _FakeKillSwitch:
    def __init__(self) -> None:
        self.active = False
        self.reason = ""

    def set(self, active: bool, reason: str) -> None:
        self.active = bool(active)
        self.reason = str(reason)


def _post(url: str, path: str, body: dict, token: str = ""):
    req = urllib.request.Request(
        url + path,
        data=json.dumps(body).encode("utf-8"),
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))


def _get(url: str, path: str, token: str = ""):
    req = urllib.request.Request(url + path, method="GET")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))


class _BaseServerTest(unittest.TestCase):
    """Bring up a server on an ephemeral port for each test. Rate
    limits are permissive so functional tests don't fight throttling;
    a dedicated test class covers throttle behaviour."""

    def setUp(self) -> None:
        self.executor = _FakeExecutor()
        self.kill_switch = _FakeKillSwitch()
        self.audit_path = Path(tempfile.mkdtemp()) / "audit.jsonl"
        self.server = control_server.ControlServer(
            tokens={
                "read-token": "read",
                "paper-token": "paper",
                "live-token": "live",
            },
            executor=self.executor,
            kill_switch=self.kill_switch,
            host="127.0.0.1",
            port=0,  # ephemeral
            audit_sink=self.audit_path,
            rate_limits={
                "orders": (100.0, 100.0),
                "cancels": (100.0, 100.0),
                "kill": (100.0, 100.0),
            },
        )
        self.server.start()
        # Replace the URL with the one bound to the actual ephemeral port.
        bound_port = self.server._http.server_address[1]
        self.server.port = bound_port

    def tearDown(self) -> None:
        self.server.stop()

    def _audit_records(self) -> List[Dict[str, Any]]:
        if not self.audit_path.exists():
            return []
        return [
            json.loads(line) for line in self.audit_path.read_text().splitlines()
            if line.strip()
        ]


class AuthAndScopeTests(_BaseServerTest):
    def test_missing_token_returns_401(self) -> None:
        status, body = _post(self.server.url, "/place_order", {})
        self.assertEqual(status, 401)
        self.assertIn("error", body)

    def test_unknown_token_returns_401(self) -> None:
        status, _ = _post(self.server.url, "/place_order", {}, token="bogus")
        self.assertEqual(status, 401)

    def test_read_scope_cannot_place_order(self) -> None:
        status, body = _post(
            self.server.url, "/place_order",
            {"account": "paper-1", "side": "buy", "qty": 1.0, "symbol": 1},
            token="read-token",
        )
        self.assertEqual(status, 403)
        self.assertIn("error", body)

    def test_health_endpoint_works_with_any_scope(self) -> None:
        status, body = _get(self.server.url, "/health", token="read-token")
        self.assertEqual(status, 200)
        self.assertEqual(body["scope"], "read")


class PaperScopeTests(_BaseServerTest):
    def test_paper_token_dry_run_does_not_submit(self) -> None:
        status, body = _post(
            self.server.url, "/place_order",
            {"account": "paper-1", "side": "buy", "qty": 1.0, "symbol": 1,
             "type": "market"},
            token="paper-token",
        )
        self.assertEqual(status, 200)
        self.assertTrue(body["dry_run"])
        self.assertEqual(self.executor.submitted, [])

    def test_paper_token_live_effect_submits_to_executor(self) -> None:
        status, body = _post(
            self.server.url, "/place_order",
            {"account": "paper-1", "side": "buy", "qty": 1.0, "symbol": 1,
             "type": "market", "dry_run": False},
            token="paper-token",
        )
        self.assertEqual(status, 200)
        self.assertFalse(body["dry_run"])
        self.assertEqual(len(self.executor.submitted), 1)
        self.assertEqual(self.executor.submitted[0]["side"], "buy")

    def test_paper_token_cannot_target_non_paper_account(self) -> None:
        status, body = _post(
            self.server.url, "/place_order",
            {"account": "bybit-prod", "side": "buy", "qty": 1.0, "symbol": 1,
             "type": "market", "dry_run": False},
            token="paper-token",
        )
        self.assertEqual(status, 403)
        self.assertIn("non-paper", body["error"])


class LiveScopeApprovalTests(_BaseServerTest):
    def test_live_place_without_approve_token_rejected(self) -> None:
        status, body = _post(
            self.server.url, "/place_order",
            {"account": "bybit-prod", "side": "buy", "qty": 0.1, "symbol": 1,
             "type": "market", "dry_run": False},
            token="live-token",
        )
        self.assertEqual(status, 403)
        self.assertIn("approve_token", body["error"])

    def test_live_place_with_fresh_approve_token_succeeds(self) -> None:
        approve = self.server.issue_approval()
        status, body = _post(
            self.server.url, "/place_order",
            {"account": "bybit-prod", "side": "buy", "qty": 0.1, "symbol": 1,
             "type": "market", "dry_run": False, "approve_token": approve},
            token="live-token",
        )
        self.assertEqual(status, 200, body)
        self.assertEqual(len(self.executor.submitted), 1)

    def test_live_approve_token_is_one_shot(self) -> None:
        approve = self.server.issue_approval()
        s1, _ = _post(
            self.server.url, "/place_order",
            {"account": "bybit-prod", "side": "buy", "qty": 0.1, "symbol": 1,
             "type": "market", "dry_run": False, "approve_token": approve},
            token="live-token",
        )
        self.assertEqual(s1, 200)
        s2, body = _post(
            self.server.url, "/place_order",
            {"account": "bybit-prod", "side": "buy", "qty": 0.1, "symbol": 1,
             "type": "market", "dry_run": False, "approve_token": approve},
            token="live-token",
        )
        self.assertEqual(s2, 403)
        self.assertIn("approve_token", body["error"])


class CancelTests(_BaseServerTest):
    def test_cancel_order(self) -> None:
        status, body = _post(
            self.server.url, "/cancel_order",
            {"order_id": 42, "dry_run": False},
            token="paper-token",
        )
        self.assertEqual(status, 200)
        self.assertEqual(self.executor.cancelled, [42])

    def test_cancel_all(self) -> None:
        status, body = _post(
            self.server.url, "/cancel_all",
            {"symbol": 7, "dry_run": False},
            token="paper-token",
        )
        self.assertEqual(status, 200)
        self.assertEqual(self.executor.cancelled_all, [7])


class FlattenTests(_BaseServerTest):
    def test_flatten_with_no_positions_accessor_is_noop(self) -> None:
        status, body = _post(
            self.server.url, "/flatten_positions",
            {"dry_run": False},
            token="paper-token",
        )
        self.assertEqual(status, 200)
        self.assertEqual(body["effects"], [])

    def test_flatten_closes_long_with_market_sell(self) -> None:
        # Wire a positions accessor in.
        self.server.positions = lambda: [
            {"symbol_id": 1, "qty": 0.5},
            {"symbol_id": 2, "qty": -1.5},
            {"symbol_id": 3, "qty": 0.0},
        ]
        status, body = _post(
            self.server.url, "/flatten_positions",
            {"dry_run": False},
            token="paper-token",
        )
        self.assertEqual(status, 200)
        # Symbol 3 has zero qty, so two effects.
        self.assertEqual(len(body["effects"]), 2)
        sides = {e["flatten"]["side"] for e in body["effects"]}
        self.assertEqual(sides, {"buy", "sell"})
        self.assertEqual(len(self.executor.submitted), 2)

    def test_flatten_with_symbol_filter(self) -> None:
        self.server.positions = lambda: [
            {"symbol_id": 1, "qty": 0.5},
            {"symbol_id": 2, "qty": -1.5},
        ]
        status, body = _post(
            self.server.url, "/flatten_positions",
            {"symbol": 2, "dry_run": False},
            token="paper-token",
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(body["effects"]), 1)
        self.assertEqual(body["effects"][0]["flatten"]["symbol"], 2)


class KillSwitchTests(_BaseServerTest):
    def test_set_kill_switch_active(self) -> None:
        status, body = _post(
            self.server.url, "/set_kill_switch",
            {"active": True, "reason": "panic", "dry_run": False},
            token="live-token",
        )
        self.assertEqual(status, 200)
        self.assertTrue(self.kill_switch.active)
        self.assertEqual(self.kill_switch.reason, "panic")


class AuditTests(_BaseServerTest):
    def test_audit_record_redacts_approve_token(self) -> None:
        approve = self.server.issue_approval()
        _post(
            self.server.url, "/place_order",
            {"account": "bybit-prod", "side": "buy", "qty": 0.1, "symbol": 1,
             "type": "market", "dry_run": False, "approve_token": approve},
            token="live-token",
        )
        records = self._audit_records()
        self.assertGreaterEqual(len(records), 1)
        place = [r for r in records if r["tool"] == "place_order"][-1]
        self.assertEqual(place["args"]["approve_token"], "<redacted>")
        self.assertTrue(place["accepted"])
        self.assertFalse(place["dry_run"])

    def test_failed_call_is_audited_too(self) -> None:
        _post(self.server.url, "/place_order", {}, token="paper-token")
        records = self._audit_records()
        self.assertGreaterEqual(len(records), 1)
        self.assertFalse(records[-1]["accepted"])
        self.assertIsNotNone(records[-1]["error"])


class RateLimitTests(unittest.TestCase):
    """Spin up a dedicated server with a tight order rate so the test
    hits the limit deterministically."""

    def setUp(self) -> None:
        self.executor = _FakeExecutor()
        self.kill = _FakeKillSwitch()
        self.server = control_server.ControlServer(
            tokens={"paper-token": "paper"},
            executor=self.executor,
            kill_switch=self.kill,
            host="127.0.0.1",
            port=0,
            rate_limits={"orders": (1.0, 0.001), "cancels": (1.0, 0.001), "kill": (5.0, 1.0)},
        )
        self.server.start()
        self.server.port = self.server._http.server_address[1]

    def tearDown(self) -> None:
        self.server.stop()

    def test_second_quick_order_is_429(self) -> None:
        body = {"account": "paper-1", "side": "buy", "qty": 1.0, "symbol": 1,
                "type": "market", "dry_run": False}
        s1, _ = _post(self.server.url, "/place_order", body, token="paper-token")
        s2, b2 = _post(self.server.url, "/place_order", body, token="paper-token")
        self.assertEqual(s1, 200)
        self.assertEqual(s2, 429, b2)
        self.assertIn("rate limit", b2["error"])


class ConfigValidationTests(unittest.TestCase):
    def test_unknown_scope_in_token_map_rejected(self) -> None:
        with self.assertRaises(ValueError):
            control_server.ControlServer(
                tokens={"x": "admin"},  # not a valid scope
                executor=_FakeExecutor(),
                kill_switch=_FakeKillSwitch(),
            )


if __name__ == "__main__":
    unittest.main()
