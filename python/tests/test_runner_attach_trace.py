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
