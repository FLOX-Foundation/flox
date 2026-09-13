"""Tests for ``flox_py.OrderBook`` tick handling and delta-fed window tracking.

Mirrors tests/test_nlevel_order_book_window.cpp through the Python binding,
which reads the same book the engine does.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_order_book_window.py
"""
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

import numpy as np  # noqa: E402

import flox_py  # noqa: E402


def _arr(values) -> np.ndarray:
    return np.array(values, dtype=np.float64)


class TickSizeValidationTests(unittest.TestCase):
    def test_zero_tick_size_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            flox_py.OrderBook(0.0)

    def test_negative_tick_size_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            flox_py.OrderBook(-0.01)

    def test_positive_tick_size_is_accepted(self) -> None:
        book = flox_py.OrderBook(0.01)
        self.assertIsNone(book.best_bid())


class TinyTickSizeTests(unittest.TestCase):
    """A 1e-8 tick is a raw divisor of 1 at a price scale of 1e8, which is what
    the sub-cent pairs quote in. Every level used to collapse onto one index."""

    def test_levels_stay_apart_at_one_satoshi(self) -> None:
        book = flox_py.OrderBook(1e-8)
        book.apply_snapshot(_arr([1e-5]), _arr([1.0]),
                            _arr([1.002e-5]), _arr([1.0]))

        self.assertAlmostEqual(book.best_bid(), 1e-5, places=12)
        self.assertAlmostEqual(book.best_ask(), 1.002e-5, places=12)
        self.assertFalse(book.is_crossed())
        self.assertAlmostEqual(book.spread(), 2e-8, places=12)
        self.assertAlmostEqual(book.mid(), 1.001e-5, places=12)

    def test_levels_stay_apart_at_two_satoshi(self) -> None:
        book = flox_py.OrderBook(2e-8)
        book.apply_snapshot(_arr([1e-5]), _arr([1.0]),
                            _arr([1.002e-5]), _arr([1.0]))

        self.assertAlmostEqual(book.best_bid(), 1e-5, places=12)
        self.assertAlmostEqual(book.best_ask(), 1.002e-5, places=12)
        self.assertFalse(book.is_crossed())


class MidTests(unittest.TestCase):
    def test_mid_is_the_average_of_both_sides(self) -> None:
        book = flox_py.OrderBook(5e-8)
        book.apply_snapshot(_arr([1e-5]), _arr([1.0]),
                            _arr([1.01e-5]), _arr([1.0]))

        expected = (book.best_bid() + book.best_ask()) / 2
        self.assertAlmostEqual(book.mid(), expected, places=12)


class DeltaDriftTests(unittest.TestCase):
    """A delta feed re-sends a snapshot only on connect and on a sequence gap,
    so the tick window has to follow the market between the two."""

    def test_book_survives_a_long_delta_only_drift(self) -> None:
        tick = 0.1
        book = flox_py.OrderBook(tick)

        bid_top, ask_top = 59999.9, 60000.1
        depth = 10
        book.apply_snapshot(
            _arr([round(bid_top - k * tick, 8) for k in range(depth)]),
            _arr([1.0] * depth),
            _arr([round(ask_top + k * tick, 8) for k in range(depth)]),
            _arr([1.0] * depth))

        for step in range(1, 601):
            new_bid = round(bid_top + step * tick, 8)
            new_ask = round(ask_top + step * tick, 8)
            book.apply_delta(
                _arr([new_bid, round(new_bid - depth * tick, 8)]), _arr([1.0, 0.0]),
                _arr([round(new_ask + (depth - 1) * tick, 8), round(new_ask - tick, 8)]),
                _arr([1.0, 0.0]))

            self.assertIsNotNone(book.best_bid(), f"bid side went dark at step {step}")
            self.assertIsNotNone(book.best_ask(), f"ask side went dark at step {step}")
            self.assertAlmostEqual(book.best_bid(), new_bid, places=6,
                                   msg=f"at step {step}")
            self.assertAlmostEqual(book.best_ask(), new_ask, places=6,
                                   msg=f"at step {step}")


if __name__ == "__main__":
    unittest.main()
