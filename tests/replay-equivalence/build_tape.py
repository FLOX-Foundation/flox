"""Deterministic tape generator for the replay-equivalence gate.

Writes a small `.floxlog` directory with a fixed sequence of synthetic
trades. The output is fully deterministic for a given seed, so the
expected_output.json frozen in this directory stays in sync as long
as nobody edits this script.
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

import flox_py as flox
from flox_py import tape


SEQUENCE = [
    # (price, qty, is_buy, exchange_ts_ns)
    (100.00, 1.0, True,  1_000_000_000),
    (100.50, 0.5, False, 1_500_000_000),
    (101.00, 0.7, True,  2_000_000_000),
    (101.25, 0.3, False, 2_500_000_000),
    (101.50, 1.2, True,  3_000_000_000),
    (101.10, 0.8, False, 3_500_000_000),
    (101.40, 0.4, True,  4_000_000_000),
]


def build_tape(out_dir: Path, *, exchange: str = "bundle",
               symbol_name: str = "BTCUSDT", tick_size: float = 0.01) -> int:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    registry = flox.SymbolRegistry()
    sym = int(registry.add_symbol(exchange, symbol_name, tick_size=tick_size))

    recorder = tape.make_recorder_hook(out_dir)
    runner = flox.Runner(registry, on_signal=lambda _sig: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    for price, qty, is_buy, ts_ns in SEQUENCE:
        runner.on_trade(sym, price, qty, is_buy, ts_ns)
    runner.stop()
    recorder.close()
    return len(SEQUENCE)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tape")
    n = build_tape(out)
    print(f"wrote tape with {n} trades to {out}")
