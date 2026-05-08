"""Tests for ``flox_py.DeltaBookEncoder`` / ``DeltaBookReplayer``."""
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


def _level(price: int, qty: int) -> dict:
    return {"price_raw": price, "qty_raw": qty}


def _normalize(side: list[dict]) -> list[tuple[int, int]]:
    return sorted((int(l["price_raw"]), int(l["qty_raw"])) for l in side
                  if int(l["qty_raw"]) > 0)


class EncoderTests(unittest.TestCase):
    def test_first_event_is_snapshot(self) -> None:
        enc = flox_py.DeltaBookEncoder(anchor_every=10)
        out = enc.encode(1,
                         [_level(10000, 10), _level(9999, 5)],
                         [_level(10001, 8)])
        self.assertFalse(out["is_delta"])
        self.assertEqual(len(out["bids"]), 2)
        self.assertEqual(len(out["asks"]), 1)

    def test_unchanged_snapshot_emits_empty_delta(self) -> None:
        enc = flox_py.DeltaBookEncoder(anchor_every=100)
        bids = [_level(10000, 10)]
        asks = [_level(10001, 5)]
        enc.encode(1, bids, asks)
        out = enc.encode(1, bids, asks)
        self.assertTrue(out["is_delta"])
        self.assertEqual(out["bids"], [])
        self.assertEqual(out["asks"], [])

    def test_anchor_cadence(self) -> None:
        enc = flox_py.DeltaBookEncoder(anchor_every=3)
        bids = [_level(10000, 10)]
        asks = [_level(10001, 5)]
        kinds = []
        for _ in range(5):
            out = enc.encode(1, bids, asks)
            kinds.append(out["is_delta"])
        # First emission is snapshot, then 2 deltas, then anchor again, then delta.
        self.assertEqual(kinds, [False, True, True, False, True])

    def test_anchor_every_zero_is_snapshot_only(self) -> None:
        enc = flox_py.DeltaBookEncoder(anchor_every=0)
        for _ in range(3):
            out = enc.encode(1, [_level(100, 1)], [_level(101, 1)])
            self.assertFalse(out["is_delta"])

    def test_delta_records_changes_and_removals(self) -> None:
        enc = flox_py.DeltaBookEncoder(anchor_every=100)
        enc.encode(1,
                   [_level(10000, 10), _level(9999, 5)],
                   [_level(10001, 8)])
        out = enc.encode(1,
                         [_level(10000, 12)],         # 9999 removed, 10000 changed
                         [_level(10001, 8), _level(10002, 3)])
        self.assertTrue(out["is_delta"])
        bid_diff = {l["price_raw"]: l["qty_raw"] for l in out["bids"]}
        self.assertEqual(bid_diff, {9999: 0, 10000: 12})
        ask_diff = {l["price_raw"]: l["qty_raw"] for l in out["asks"]}
        self.assertEqual(ask_diff, {10002: 3})


class ReplayerTests(unittest.TestCase):
    def test_replayer_reconstructs_snapshot_after_delta(self) -> None:
        enc = flox_py.DeltaBookEncoder(anchor_every=100)
        rep = flox_py.DeltaBookReplayer()

        sequences = [
            ([_level(10000, 10), _level(9999, 5)], [_level(10001, 8)]),
            ([_level(10000, 12)], [_level(10001, 8), _level(10002, 3)]),
            ([_level(10000, 12), _level(9998, 1)], [_level(10001, 7)]),
        ]

        for bids, asks in sequences:
            ev = enc.encode(1, bids, asks)
            kind = 1 if ev["is_delta"] else 0
            replayed = rep.apply(kind, 1, ev["bids"], ev["asks"])
            self.assertEqual(_normalize(replayed["bids"]),
                             _normalize(bids),
                             f"bids mismatch on {bids}")
            self.assertEqual(_normalize(replayed["asks"]),
                             _normalize(asks),
                             f"asks mismatch on {asks}")

    def test_size_reduction_versus_snapshot_only(self) -> None:
        # Build a 50-level book that mutates a few levels each step.
        bids = [_level(10000 - i, 100 - i) for i in range(50)]
        asks = [_level(10001 + i, 100 - i) for i in range(50)]

        delta_enc = flox_py.DeltaBookEncoder(anchor_every=20)
        snap_enc = flox_py.DeltaBookEncoder(anchor_every=0)

        delta_levels = 0
        snap_levels = 0
        for step in range(100):
            # Mutate three levels per step.
            cur_bids = list(bids)
            cur_bids[step % 50] = _level(10000 - (step % 50), 100 - (step % 50) + step % 7)
            d = delta_enc.encode(1, cur_bids, asks)
            s = snap_enc.encode(1, cur_bids, asks)
            delta_levels += len(d["bids"]) + len(d["asks"])
            snap_levels += len(s["bids"]) + len(s["asks"])

        # Delta encoding should send dramatically fewer levels than
        # snapshot-only. The exact ratio depends on mutation rate; this
        # check is loose to keep the test stable.
        self.assertLess(delta_levels * 5, snap_levels,
                        f"delta {delta_levels} vs snap {snap_levels}")


if __name__ == "__main__":
    unittest.main()
