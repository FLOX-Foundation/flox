"""Tests for live strategy hot-reload via runner.replace_strategy."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import flox_py  # noqa: E402


class _RecordingStrategy(flox_py.Strategy):
    """Marks each lifecycle event so a test can assert who saw what."""

    def __init__(self, symbols, label: str) -> None:
        super().__init__(symbols)
        self.label = label
        self.starts = 0
        self.stops = 0
        self.trades: list[tuple[str, float]] = []

    def on_start(self) -> None:
        self.starts += 1

    def on_stop(self) -> None:
        self.stops += 1

    def on_trade(self, ctx, trade) -> None:
        self.trades.append((self.label, trade.price))


class HotReloadTests(unittest.TestCase):
    def test_replace_strategy_swaps_callbacks(self) -> None:
        registry = flox_py.SymbolRegistry()
        sym = int(registry.add_symbol("test", "BTCUSDT", tick_size=0.01))

        runner = flox_py.Runner(registry, on_signal=lambda _: None)
        a = _RecordingStrategy([sym], "A")
        runner.add_strategy(a)
        runner.start()

        runner.on_trade(sym, 100.0, 1.0, True, 1_000_000)
        self.assertEqual(len(a.trades), 1)
        self.assertEqual(a.trades[0], ("A", 100.0))

        b = _RecordingStrategy([sym], "B")
        runner.replace_strategy(0, b)

        runner.on_trade(sym, 101.0, 1.0, False, 2_000_000)
        runner.stop()

        # Old strategy received its first trade; the second goes to B.
        self.assertEqual([t[0] for t in a.trades], ["A"])
        self.assertEqual([t[0] for t in b.trades], ["B"])
        self.assertEqual(b.trades[0][1], 101.0)

        # Lifecycle: A.start fired on add, A.stop on replace, B.start
        # on replace, B.stop on runner.stop.
        self.assertEqual(a.starts, 1)
        self.assertEqual(a.stops, 1)
        self.assertEqual(b.starts, 1)
        self.assertEqual(b.stops, 1)

    def test_replace_strategy_rejects_bad_index(self) -> None:
        registry = flox_py.SymbolRegistry()
        sym = int(registry.add_symbol("test", "BTCUSDT", tick_size=0.01))
        runner = flox_py.Runner(registry, on_signal=lambda _: None)
        a = _RecordingStrategy([sym], "A")
        runner.add_strategy(a)

        b = _RecordingStrategy([sym], "B")
        with self.assertRaises(Exception):
            runner.replace_strategy(99, b)


if __name__ == "__main__":
    unittest.main()
