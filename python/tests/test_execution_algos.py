"""Tests for ``flox_py.execution_algos``."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from typing import Any, Dict, List

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import execution_algos as ea  # noqa: E402


class _RecordingExecutor:
    """Implements the minimal ``submit_order`` contract."""

    def __init__(self) -> None:
        self.calls: List[Dict[str, Any]] = []

    def submit_order(self, id_, side, price, qty, type, symbol):  # noqa: A002
        self.calls.append({
            "id": int(id_),
            "side": side,
            "price": float(price),
            "qty": float(qty),
            "type": type,
            "symbol": int(symbol),
        })


# ── TWAP ──────────────────────────────────────────────────────────


class TWAPTests(unittest.TestCase):
    def test_emits_one_slice_per_tick(self) -> None:
        algo = ea.TWAPExecutor(
            target_qty=10.0, side="buy", symbol=1,
            duration_ns=10_000_000_000,  # 10s
            slice_count=5,
            start_time_ns=0,
        )
        ex = _RecordingExecutor()
        # Tick at 0: first slice fires.
        out = algo.step(0, ex)
        self.assertEqual(len(out), 1)
        self.assertAlmostEqual(out[0].qty, 2.0)
        # Tick at 1ns: not yet time for second slice.
        out = algo.step(1, ex)
        self.assertEqual(out, [])
        # Tick at slice_interval: second slice fires.
        out = algo.step(algo.slice_interval_ns, ex)
        self.assertEqual(len(out), 1)

    def test_completes_after_all_slices(self) -> None:
        algo = ea.TWAPExecutor(
            target_qty=8.0, side="sell", symbol=1,
            duration_ns=4_000_000_000, slice_count=4, start_time_ns=0,
        )
        ex = _RecordingExecutor()
        # Advance well past the duration; all 4 slices should fire at once.
        algo.step(10_000_000_000, ex)
        self.assertTrue(algo.is_done())
        self.assertEqual(len(ex.calls), 4)
        self.assertAlmostEqual(sum(c["qty"] for c in ex.calls), 8.0)

    def test_validates_args(self) -> None:
        with self.assertRaises(ValueError):
            ea.TWAPExecutor(target_qty=-1.0, side="buy", symbol=1,
                            duration_ns=1_000, slice_count=1)
        with self.assertRaises(ValueError):
            ea.TWAPExecutor(target_qty=1.0, side="upside", symbol=1,
                            duration_ns=1_000, slice_count=1)
        with self.assertRaises(ValueError):
            ea.TWAPExecutor(target_qty=1.0, side="buy", symbol=1,
                            duration_ns=0, slice_count=1)
        with self.assertRaises(ValueError):
            ea.TWAPExecutor(target_qty=1.0, side="buy", symbol=1,
                            duration_ns=1_000, slice_count=0)


# ── VWAP ──────────────────────────────────────────────────────────


class VWAPTests(unittest.TestCase):
    def test_slices_proportional_to_volume(self) -> None:
        # 4 bars, volumes 100/300/100/100 → shares 1/6, 1/2, 1/6, 1/6
        algo = ea.VWAPExecutor(
            target_qty=600.0, side="buy", symbol=1,
            volume_curve=[
                (1_000, 100.0),
                (2_000, 300.0),
                (3_000, 100.0),
                (4_000, 100.0),
            ],
        )
        ex = _RecordingExecutor()
        algo.step(5_000, ex)  # past every bar
        qtys = [c["qty"] for c in ex.calls]
        self.assertEqual(len(qtys), 4)
        self.assertAlmostEqual(qtys[0], 100.0)
        self.assertAlmostEqual(qtys[1], 300.0)
        self.assertAlmostEqual(qtys[2], 100.0)
        self.assertAlmostEqual(qtys[3], 100.0)
        self.assertTrue(algo.is_done())

    def test_zero_volume_bar_skipped(self) -> None:
        algo = ea.VWAPExecutor(
            target_qty=200.0, side="sell", symbol=1,
            volume_curve=[(100, 100.0), (200, 0.0), (300, 100.0)],
        )
        ex = _RecordingExecutor()
        algo.step(1_000, ex)
        self.assertEqual(len(ex.calls), 2)

    def test_empty_curve_rejected(self) -> None:
        with self.assertRaises(ValueError):
            ea.VWAPExecutor(target_qty=1.0, side="buy", symbol=1,
                            volume_curve=[])

    def test_negative_volume_rejected(self) -> None:
        with self.assertRaises(ValueError):
            ea.VWAPExecutor(target_qty=1.0, side="buy", symbol=1,
                            volume_curve=[(1, -1.0)])


# ── Iceberg ───────────────────────────────────────────────────────


class IcebergTests(unittest.TestCase):
    def test_resubmits_after_fill(self) -> None:
        algo = ea.IcebergExecutor(
            target_qty=10.0, side="buy", symbol=1,
            visible_qty=2.0,
        )
        ex = _RecordingExecutor()
        # First step submits one visible slice.
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 1)
        self.assertAlmostEqual(ex.calls[0]["qty"], 2.0)
        # Without a fill report, no more slices.
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 1)
        # Report a fill; next step submits the next slice.
        algo.report_fill(2.0)
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 2)

    def test_last_slice_can_be_smaller_than_visible(self) -> None:
        algo = ea.IcebergExecutor(
            target_qty=5.0, side="sell", symbol=1,
            visible_qty=2.0,
        )
        ex = _RecordingExecutor()
        algo.step(0, ex); algo.report_fill(2.0)
        algo.step(0, ex); algo.report_fill(2.0)
        algo.step(0, ex); algo.report_fill(1.0)
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 3)
        self.assertAlmostEqual(ex.calls[-1]["qty"], 1.0)
        self.assertTrue(algo.is_done())

    def test_visible_qty_validation(self) -> None:
        with self.assertRaises(ValueError):
            ea.IcebergExecutor(target_qty=1.0, side="buy", symbol=1,
                               visible_qty=0)
        with self.assertRaises(ValueError):
            ea.IcebergExecutor(target_qty=1.0, side="buy", symbol=1,
                               visible_qty=2.0)  # exceeds target


# ── POV ───────────────────────────────────────────────────────────


class POVTests(unittest.TestCase):
    def test_chases_observed_volume(self) -> None:
        algo = ea.POVExecutor(
            target_qty=10.0, side="buy", symbol=1,
            participation_rate=0.10, min_slice_qty=0.0,
        )
        ex = _RecordingExecutor()
        # No volume yet, no slice.
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 0)
        # Observe 50.0 of market volume, target = 5.0 cumulative submit.
        algo.observe_volume(50.0)
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 1)
        self.assertAlmostEqual(ex.calls[0]["qty"], 5.0)
        # More volume → next slice catches the gap.
        algo.observe_volume(50.0)
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 2)
        self.assertAlmostEqual(ex.calls[1]["qty"], 5.0)
        self.assertTrue(algo.is_done())

    def test_min_slice_holds_back_tiny_slices(self) -> None:
        algo = ea.POVExecutor(
            target_qty=10.0, side="buy", symbol=1,
            participation_rate=0.10, min_slice_qty=2.0,
        )
        ex = _RecordingExecutor()
        algo.observe_volume(15.0)  # would-be slice = 1.5, below 2.0
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 0)
        algo.observe_volume(15.0)  # cumulative slice = 3.0 → fires
        algo.step(0, ex)
        self.assertEqual(len(ex.calls), 1)

    def test_participation_rate_validation(self) -> None:
        with self.assertRaises(ValueError):
            ea.POVExecutor(target_qty=1.0, side="buy", symbol=1,
                           participation_rate=0.0)
        with self.assertRaises(ValueError):
            ea.POVExecutor(target_qty=1.0, side="buy", symbol=1,
                           participation_rate=1.5)


# ── Common ─────────────────────────────────────────────────────────


class FillReportingTests(unittest.TestCase):
    def test_report_fill_updates_filled_qty(self) -> None:
        algo = ea.TWAPExecutor(
            target_qty=4.0, side="buy", symbol=1,
            duration_ns=1_000, slice_count=2, start_time_ns=0,
        )
        algo.report_fill(1.5)
        self.assertAlmostEqual(algo.filled_qty, 1.5)

    def test_negative_fill_rejected(self) -> None:
        algo = ea.TWAPExecutor(
            target_qty=4.0, side="buy", symbol=1,
            duration_ns=1_000, slice_count=2, start_time_ns=0,
        )
        with self.assertRaises(ValueError):
            algo.report_fill(-1.0)


if __name__ == "__main__":
    unittest.main()
