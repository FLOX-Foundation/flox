"""Tests for ``flox_py.event_log.EventLog``."""
from __future__ import annotations

import sys
import threading
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import event_log  # noqa: E402


class EmitQueryTests(unittest.TestCase):
    def test_emit_returns_record_with_assigned_event_id(self) -> None:
        log = event_log.EventLog()
        rec = log.emit("signal", strategy="ema-trend", payload={"side": "buy"})
        self.assertTrue(rec.event_id)
        self.assertEqual(rec.type, "signal")
        self.assertEqual(rec.payload["side"], "buy")
        self.assertEqual(len(log), 1)

    def test_query_filters_compose(self) -> None:
        log = event_log.EventLog()
        log.emit("signal", strategy="a")
        log.emit("order", strategy="a")
        log.emit("signal", strategy="b")
        self.assertEqual(len(log.query()), 3)
        self.assertEqual(len(log.query(strategy="a")), 2)
        self.assertEqual(len(log.query(type="signal")), 2)
        self.assertEqual(len(log.query(strategy="a", type="signal")), 1)

    def test_query_respects_time_range(self) -> None:
        log = event_log.EventLog()
        log.emit("a", timestamp_ns=1000)
        log.emit("b", timestamp_ns=2000)
        log.emit("c", timestamp_ns=3000)
        self.assertEqual(len(log.query(from_ts_ns=1500, to_ts_ns=2500)), 1)

    def test_query_limit(self) -> None:
        log = event_log.EventLog()
        for i in range(10):
            log.emit("x", payload={"i": i})
        self.assertEqual(len(log.query(limit=3)), 3)


class CapacityTests(unittest.TestCase):
    def test_oldest_records_drop_at_capacity(self) -> None:
        log = event_log.EventLog(capacity=3)
        for i in range(5):
            log.emit("x", payload={"i": i})
        ids = [r.payload["i"] for r in log.query()]
        self.assertEqual(ids, [2, 3, 4])

    def test_zero_capacity_rejected(self) -> None:
        with self.assertRaises(ValueError):
            event_log.EventLog(capacity=0)


class CausalTraceTests(unittest.TestCase):
    def test_trace_walks_chain_to_root(self) -> None:
        log = event_log.EventLog()
        root = log.emit("trade")
        sig = log.emit("signal", causal_parent_id=root.event_id)
        order = log.emit("order", causal_parent_id=sig.event_id)
        fill = log.emit("fill", causal_parent_id=order.event_id)

        chain = log.trace(fill.event_id)
        self.assertEqual([r.type for r in chain], ["fill", "order", "signal", "trade"])

    def test_trace_unknown_event_id_returns_empty(self) -> None:
        log = event_log.EventLog()
        log.emit("x")
        self.assertEqual(log.trace("nonexistent"), [])

    def test_trace_max_depth(self) -> None:
        log = event_log.EventLog()
        prev = log.emit("seed")
        for _ in range(50):
            prev = log.emit("step", causal_parent_id=prev.event_id)
        chain = log.trace(prev.event_id, max_depth=10)
        self.assertEqual(len(chain), 10)


class ConcurrencyTests(unittest.TestCase):
    def test_emit_from_many_threads_does_not_corrupt(self) -> None:
        log = event_log.EventLog(capacity=10_000)

        def worker(n: int) -> None:
            for i in range(n):
                log.emit("x", payload={"i": i})

        threads = [threading.Thread(target=worker, args=(200,)) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(len(log), 1600)


if __name__ == "__main__":
    unittest.main()
