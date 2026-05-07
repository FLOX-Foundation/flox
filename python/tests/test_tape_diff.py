"""Tests for ``flox tape diff`` and the underlying ``diff_tapes``."""
from __future__ import annotations

import shutil
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

import flox_py as flox  # noqa: E402

from flox_py import tape  # noqa: E402


def _write_tape(out_dir: Path, trades) -> int:
    """trades: iterable of (price, qty, is_buy, ts_ns). Returns count."""
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    registry = flox.SymbolRegistry()
    sym = int(registry.add_symbol("diff", "BTCUSDT", tick_size=0.01))
    recorder = tape.make_recorder_hook(out_dir)
    runner = flox.Runner(registry, on_signal=lambda _sig: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    n = 0
    for price, qty, is_buy, ts_ns in trades:
        runner.on_trade(sym, price, qty, is_buy, ts_ns)
        n += 1
    runner.stop()
    recorder.close()
    return n


_BASE = [
    (100.0, 1.0, True,  1_000_000_000),
    (100.5, 0.5, False, 1_500_000_000),
    (101.0, 1.5, True,  2_000_000_000),
]


class EqualTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="tape-diff-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_two_identical_tapes_are_equal(self) -> None:
        a, b = self.work / "a", self.work / "b"
        _write_tape(a, _BASE)
        _write_tape(b, _BASE)
        d = tape.diff_tapes(a, b)
        self.assertTrue(d.equal, f"unexpected diff: {d}")
        self.assertEqual(d.first_divergence_index, None)
        self.assertEqual(d.mismatches, [])
        self.assertEqual(d.left_count, len(_BASE))
        self.assertEqual(d.right_count, len(_BASE))


class DivergenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="tape-diff-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_price_change_shows_first_divergence(self) -> None:
        a, b = self.work / "a", self.work / "b"
        _write_tape(a, _BASE)
        modified = list(_BASE)
        modified[1] = (modified[1][0] + 0.01, modified[1][1], modified[1][2], modified[1][3])
        _write_tape(b, modified)
        d = tape.diff_tapes(a, b)
        self.assertFalse(d.equal)
        self.assertEqual(d.first_divergence_index, 1)
        self.assertGreaterEqual(len(d.mismatches), 1)
        self.assertNotEqual(
            d.mismatches[0]["left"]["price_raw"],
            d.mismatches[0]["right"]["price_raw"],
        )

    def test_extra_trade_in_one_tape(self) -> None:
        a, b = self.work / "a", self.work / "b"
        _write_tape(a, _BASE)
        _write_tape(b, _BASE + [(102.0, 0.7, True, 3_000_000_000)])
        d = tape.diff_tapes(a, b)
        self.assertFalse(d.equal)
        self.assertEqual(d.left_count, len(_BASE))
        self.assertEqual(d.right_count, len(_BASE) + 1)
        # First-divergence index points at the row past the shared
        # prefix when the prefix was equal.
        self.assertEqual(d.first_divergence_index, len(_BASE))


class MaxMismatchesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="tape-diff-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_caps_recorded_mismatches(self) -> None:
        a, b = self.work / "a", self.work / "b"
        # 10 trades vs 10 trades, every row differs in price.
        left = [(100.0 + i, 1.0, True, 1_000_000_000 + i * 100) for i in range(10)]
        right = [(200.0 + i, 1.0, True, 1_000_000_000 + i * 100) for i in range(10)]
        _write_tape(a, left)
        _write_tape(b, right)
        d = tape.diff_tapes(a, b, max_mismatches=3)
        self.assertEqual(d.first_divergence_index, 0)
        self.assertEqual(len(d.mismatches), 3)


class TimestampToleranceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="tape-diff-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_within_tolerance_is_equal(self) -> None:
        a, b = self.work / "a", self.work / "b"
        _write_tape(a, _BASE)
        # shift each timestamp by 100ns
        shifted = [(p, q, s, ts + 100) for (p, q, s, ts) in _BASE]
        _write_tape(b, shifted)
        # zero tolerance: divergent
        d = tape.diff_tapes(a, b, field_tolerance_ns=0)
        self.assertFalse(d.equal)
        # 1us tolerance: equal
        d = tape.diff_tapes(a, b, field_tolerance_ns=1_000)
        self.assertTrue(d.equal, f"expected equal under tolerance, got {d}")


if __name__ == "__main__":
    unittest.main()
