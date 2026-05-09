"""Verify Runner.attach_trace_recorder() surface (W14-T012).

The runner mirrors emitted signals into a TraceRecorder when one is
attached. Driving real signals through the C ABI from a unit test is
heavy, so this suite verifies the attach / detach surface is reachable
and the per-tick `feed_ts_ns` setter is wired. End-to-end signal
capture is exercised through the QuickJS / Codon parity path on a
running engine.
"""
from __future__ import annotations

import tempfile
import time
from pathlib import Path

import flox_py


def test_attach_and_detach_round_trip() -> None:
    with tempfile.TemporaryDirectory() as d:
        runner_path = Path(d) / "run.floxrun"
        rec = flox_py.TraceRecorder(
            path=str(runner_path),
            strategy_id="attach-test",
            strategy_hash="sha256:test",
            run_started_ns=time.time_ns(),
        )
        reg = flox_py.SymbolRegistry()
        runner = flox_py.Runner(reg, on_signal=lambda sig: None)

        # Attach + detach + reattach must all be no-throw.
        runner.attach_trace_recorder(rec)
        runner.attach_trace_recorder(None)
        runner.attach_trace_recorder(rec)


def test_set_trace_feed_ts_ns_accepts_int() -> None:
    reg = flox_py.SymbolRegistry()
    runner = flox_py.Runner(reg, on_signal=lambda sig: None)
    runner.set_trace_feed_ts_ns(123_456_789)
    runner.set_trace_feed_ts_ns(0)


def test_trace_order_event_and_fill_round_trip() -> None:
    """T013 — `Runner.trace_order_event` and `trace_fill` write into the
    attached recorder so a round-trip read sees the records back."""
    with tempfile.TemporaryDirectory() as d:
        runner_path = Path(d) / "run.floxrun"
        rec = flox_py.TraceRecorder(
            path=str(runner_path),
            strategy_id="trace-events",
            strategy_hash="sha256:test",
            run_started_ns=time.time_ns(),
        )
        reg = flox_py.SymbolRegistry()
        runner = flox_py.Runner(reg, on_signal=lambda sig: None)
        runner.attach_trace_recorder(rec)

        # event_kind: 0=Submit, 6=Fill (matches OrderEventKind).
        runner.trace_order_event(order_id=42, parent_signal_id=1, symbol_id=10,
                                  event_kind=0, side=0, order_type=1,
                                  price=50000.0, qty=0.1)
        runner.trace_fill(order_id=42, fill_id=99, price=50000.5, qty=0.1,
                           fee=0.5, symbol_id=10, side=0, liquidity=2)
        rec.close()

        reader = flox_py.TraceReader(str(runner_path))
        orders = reader.read_all_order_events()
        fills = reader.read_all_fills()
        assert len(orders) == 1
        assert len(fills) == 1
        assert fills[0]["order_id"] == 42
        assert fills[0]["fill_id"] == 99
        assert fills[0]["symbol_id"] == 10


def test_trace_methods_no_op_without_recorder() -> None:
    """When no recorder is attached the methods must be a no-op (no
    crash, no thrown exception)."""
    reg = flox_py.SymbolRegistry()
    runner = flox_py.Runner(reg, on_signal=lambda sig: None)
    runner.trace_order_event(order_id=1, parent_signal_id=0, symbol_id=1,
                              event_kind=0, side=0, order_type=1,
                              price=100.0, qty=0.1)
    runner.trace_fill(order_id=1, fill_id=1, price=100.0, qty=0.1, fee=0.0,
                       symbol_id=1, side=0, liquidity=0)
