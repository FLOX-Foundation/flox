"""Tests for ``flox_py.panel`` — cross-sectional (T × S) panel builders.

Per-symbol floxlog tapes are constructed in a tempdir with synthetic
trades that bucket cleanly under a 1-minute aggregation. Three
alignment modes are exercised end-to-end (intersection / union_nan /
union_ffill) by deliberately giving one symbol a different bucket
coverage than the others.

Covers:

  * `build_close_panel` shape, ordering, intersection mode.
  * `union_nan` / `union_ffill` semantics on a tape that skips a bucket.
  * `build_ohlc_panel` returns all five (T, S) arrays aligned.
  * `build_returns_panel` returns simple-return shape with NaN warmup.
  * Symbol order in the input is preserved as the column order.
  * Bad alignment mode raises.
  * `tape_paths={}` (explicit per-symbol path mapping) works alongside
    the default `tape_root/SYMBOL` layout.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_panel.py
"""
from __future__ import annotations

import sys
import tempfile
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

from flox_py import panel as panel_mod  # noqa: E402


_BUCKET_NS = 60_000_000_000   # 1 minute


def _write_tape(root: Path, symbol: str, *,
                bucket_starts_ms: list[int],
                base_price: float) -> Path:
    """Write a tape with one trade per requested bucket so the resulting
    OHLC has exactly one row per `bucket_starts_ms` entry."""
    tape_dir = root / symbol
    tape_dir.mkdir(parents=True, exist_ok=True)
    w = flox_py.DataWriter(str(tape_dir), max_segment_mb=64,
                           exchange_id=0, compression="none")
    try:
        for i, ts_ms in enumerate(bucket_starts_ms):
            ts_ns = int(ts_ms) * 1_000_000
            # Place the trade somewhere inside the bucket window so it
            # aggregates cleanly. Use price = base + i * 0.5 so panels
            # have distinguishable values.
            price = base_price + i * 0.5
            w.write_trade(
                exchange_ts_ns=ts_ns + 1_000_000,  # +1ms inside bucket
                recv_ts_ns=ts_ns + 1_000_000,
                price=float(price),
                qty=1.0,
                trade_id=10_000 + i,
                symbol_id=1,
                side=0,
            )
    finally:
        w.close()
    return tape_dir


class PanelBuilderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="panel-"))
        # Three symbols. BTC and ETH share five buckets [0..4]; SOL
        # skips bucket 2 so union/intersection differ.
        base_ms = 1_700_000_000_000
        self.bucket_full = [base_ms + i * 60_000 for i in range(5)]
        self.bucket_sparse = [base_ms + i * 60_000 for i in [0, 1, 3, 4]]
        _write_tape(self.tmp, "BTCUSDT",
                    bucket_starts_ms=self.bucket_full, base_price=40_000.0)
        _write_tape(self.tmp, "ETHUSDT",
                    bucket_starts_ms=self.bucket_full, base_price=2_500.0)
        _write_tape(self.tmp, "SOLUSDT",
                    bucket_starts_ms=self.bucket_sparse, base_price=150.0)

    def test_close_panel_intersection(self) -> None:
        p = panel_mod.build_close_panel(
            ["BTCUSDT", "ETHUSDT", "SOLUSDT"],
            bucket_ns=_BUCKET_NS,
            tape_root=self.tmp,
            align="intersection",
        )
        # SOL skips bucket 2 → intersection keeps 4 buckets.
        self.assertEqual(p.values.shape, (4, 3))
        self.assertEqual(p.symbols, ["BTCUSDT", "ETHUSDT", "SOLUSDT"])
        self.assertEqual(p.mode, "intersection")
        # No NaN under intersection.
        self.assertFalse(np.isnan(p.values).any())
        # Time index must be sorted ascending.
        self.assertTrue(np.all(np.diff(p.ts) > 0))

    def test_close_panel_union_nan(self) -> None:
        p = panel_mod.build_close_panel(
            ["BTCUSDT", "ETHUSDT", "SOLUSDT"],
            bucket_ns=_BUCKET_NS,
            tape_root=self.tmp,
            align="union_nan",
        )
        # Union = 5 distinct buckets (the SOL gap is included).
        self.assertEqual(p.values.shape, (5, 3))
        # SOL column has a NaN at the skipped bucket.
        sol_col = p.values[:, 2]
        self.assertEqual(int(np.sum(np.isnan(sol_col))), 1)
        # Other columns are fully populated.
        self.assertFalse(np.isnan(p.values[:, :2]).any())

    def test_close_panel_union_ffill(self) -> None:
        p = panel_mod.build_close_panel(
            ["BTCUSDT", "ETHUSDT", "SOLUSDT"],
            bucket_ns=_BUCKET_NS,
            tape_root=self.tmp,
            align="union_ffill",
        )
        self.assertEqual(p.values.shape, (5, 3))
        # Forward-fill closes the SOL gap: bucket-2 value equals bucket-1.
        sol_col = p.values[:, 2]
        self.assertFalse(np.isnan(sol_col).any())
        self.assertEqual(float(sol_col[2]), float(sol_col[1]))

    def test_ohlc_panel_shape(self) -> None:
        op = panel_mod.build_ohlc_panel(
            ["BTCUSDT", "ETHUSDT"],
            bucket_ns=_BUCKET_NS,
            tape_root=self.tmp,
            align="intersection",
        )
        for attr in ("open", "high", "low", "close"):
            arr = getattr(op, attr)
            self.assertEqual(arr.shape, (5, 2), f"{attr} shape")
            self.assertFalse(np.isnan(arr).any(), f"{attr} has NaN")
        # high >= low at every cell.
        self.assertTrue((op.high >= op.low).all())

    def test_returns_panel(self) -> None:
        rp = panel_mod.build_returns_panel(
            ["BTCUSDT", "ETHUSDT"],
            bucket_ns=_BUCKET_NS,
            tape_root=self.tmp,
            lookback_n=2,
            align="intersection",
        )
        self.assertEqual(rp.values.shape, (5, 2))
        # First `lookback_n` rows are NaN warmup.
        self.assertTrue(np.isnan(rp.values[:2]).all())
        # The rest are finite numbers.
        self.assertFalse(np.isnan(rp.values[2:]).any())
        self.assertEqual(rp.lookback_n, 2)

    def test_symbol_order_preserved(self) -> None:
        # Reverse the input order; column order must follow.
        p1 = panel_mod.build_close_panel(
            ["BTCUSDT", "ETHUSDT"],
            bucket_ns=_BUCKET_NS, tape_root=self.tmp,
            align="intersection",
        )
        p2 = panel_mod.build_close_panel(
            ["ETHUSDT", "BTCUSDT"],
            bucket_ns=_BUCKET_NS, tape_root=self.tmp,
            align="intersection",
        )
        np.testing.assert_allclose(p1.values[:, 0], p2.values[:, 1])
        np.testing.assert_allclose(p1.values[:, 1], p2.values[:, 0])

    def test_tape_paths_mapping(self) -> None:
        # Explicit per-symbol path: same outcome as tape_root layout.
        p = panel_mod.build_close_panel(
            ["BTCUSDT"],
            bucket_ns=_BUCKET_NS,
            tape_paths={"BTCUSDT": self.tmp / "BTCUSDT"},
            align="intersection",
        )
        self.assertEqual(p.values.shape, (5, 1))
        self.assertEqual(p.symbols, ["BTCUSDT"])

    def test_bad_mode_raises(self) -> None:
        with self.assertRaises(ValueError):
            panel_mod.build_close_panel(
                ["BTCUSDT"],
                bucket_ns=_BUCKET_NS, tape_root=self.tmp,
                align="bogus",
            )


if __name__ == "__main__":
    unittest.main()
