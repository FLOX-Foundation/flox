#!/usr/bin/env python3
"""Produce a deterministic .floxrun directory through the pybind11 binding.

Used by `scripts/floxrun_byte_identical_gate.py` — every binding writes
the same fixture, the gate hashes the segment files and asserts they
match byte-for-byte.

The fixture is intentionally small and uses fully explicit timestamps
so the only knob that could cause divergence is the recorder's own
serialization path.

Usage: python3 scripts/floxrun_byte_identical_fixture.py <out-dir>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "build" / "python"))

import flox_py


def write_fixture(out_dir: str) -> None:
    rec = flox_py.TraceRecorder(
        out_dir,
        strategy_id="byte-identical-gate",
        strategy_hash="sha256:fixture",
        run_started_ns=1_700_000_000_000_000_000,
    )
    rec.write_signal(
        run_ts_ns=1_700_000_000_100_000_000,
        feed_ts_ns=1_700_000_000_099_000_000,
        signal_id=42,
        flags=flox_py._flox_py.SIGNAL_FLAG_ENTER,
        strength_raw=75_000_000,
        name="entry",
        symbol_ids=[1, 2],
        payload=b"",
    )
    rec.write_signal(
        run_ts_ns=1_700_000_000_200_000_000,
        feed_ts_ns=1_700_000_000_199_000_000,
        signal_id=43,
        flags=flox_py._flox_py.SIGNAL_FLAG_EXIT,
        strength_raw=0,
        name="exit",
        symbol_ids=[1],
        payload=b"",
    )
    rec.write_order_event(
        run_ts_ns=1_700_000_000_150_000_000,
        feed_ts_ns=1_700_000_000_149_000_000,
        order_id=7,
        parent_signal_id=42,
        price_raw=50_000_00000000,
        qty_raw=100_000_000,
        symbol_id=1,
        event_kind=flox_py.OrderEventKind.SUBMIT,
        side=0,
        order_type=1,
        flags=0,
        reason="",
    )
    rec.write_fill(
        run_ts_ns=1_700_000_000_175_000_000,
        feed_ts_ns=1_700_000_000_174_000_000,
        order_id=7,
        fill_id=12345,
        price_raw=50_000_00000000,
        qty_raw=100_000_000,
        fee_raw=50_000,
        symbol_id=1,
        side=0,
        liquidity=flox_py.FillLiquidity.MAKER,
    )
    rec.set_run_ended_ns(1_700_000_000_400_000_000)
    rec.close()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <out-dir>", file=sys.stderr)
        sys.exit(2)
    write_fixture(sys.argv[1])
