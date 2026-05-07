"""Tests for the reproducibility bundle (W6.T015)."""
from __future__ import annotations

import json
import shutil
import sys
import tarfile
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

from flox_py import bundle, tape  # noqa: E402


_STRATEGY_SOURCE = '''
"""A trivial strategy: fire one market BUY on the first trade."""
import flox_py as flox


class FirstTradeBuy(flox.Strategy):
    def __init__(self, symbols, qty=1.0):
        super().__init__(symbols)
        self.qty = qty
        self._fired = False

    def on_trade(self, ctx, trade):
        if self._fired:
            return
        self._fired = True
        self.market_buy(self.qty)
'''


def _write_strategy(dir_: Path) -> Path:
    p = dir_ / "strategy.py"
    p.write_text(_STRATEGY_SOURCE)
    return p


def _write_tape(dir_: Path) -> Path:
    """Capture a synthetic tape with three trades."""
    out = dir_ / "tape"
    out.mkdir()
    registry = flox.SymbolRegistry()
    sym = int(registry.add_symbol("bundle", "BTCUSDT", tick_size=0.01))

    recorder = tape.make_recorder_hook(out)
    runner = flox.Runner(registry, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    runner.on_trade(sym, 100.00, 1.0, True, 1_000_000_000)
    runner.on_trade(sym, 100.50, 0.5, False, 2_000_000_000)
    runner.on_trade(sym, 101.00, 1.5, True, 3_000_000_000)
    runner.stop()
    recorder.close()
    return out


class BundlePackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="flox-bundle-test-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_pack_creates_tarball_with_required_layout(self) -> None:
        strat = _write_strategy(self.work)
        tape_dir = _write_tape(self.work)
        out = self.work / "bundle.tar"

        bundle.pack_bundle(
            strategy=strat,
            tape=tape_dir,
            output=out,
        )

        self.assertTrue(out.is_file())
        with tarfile.open(out) as tf:
            names = set(tf.getnames())

        self.assertIn("manifest.json", names)
        self.assertIn("strategy/strategy.py", names)
        self.assertIn("config/params.json", names)
        self.assertIn("expected_output.json", names)
        self.assertTrue(any(n.startswith("tape/") for n in names))

    def test_pack_manifest_contains_required_fields(self) -> None:
        strat = _write_strategy(self.work)
        tape_dir = _write_tape(self.work)
        out = self.work / "bundle.tar"

        bundle.pack_bundle(strategy=strat, tape=tape_dir, output=out)

        with tarfile.open(out) as tf:
            manifest = json.loads(tf.extractfile("manifest.json").read())

        self.assertEqual(manifest["bundle_format_version"], 1)
        self.assertIn("flox_version", manifest)
        self.assertIn("created_at_ns", manifest)
        self.assertEqual(manifest["strategy_filename"], "strategy.py")
        self.assertIn("strategy_sha256", manifest)
        self.assertIn("tape_sha256", manifest)


class BundleReplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="flox-bundle-test-"))
        strat = _write_strategy(self.work)
        tape_dir = _write_tape(self.work)
        self.bundle_path = self.work / "bundle.tar"
        bundle.pack_bundle(
            strategy=strat,
            tape=tape_dir,
            output=self.bundle_path,
        )

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_replay_returns_actual_plus_expected(self) -> None:
        res = bundle.replay_bundle(self.bundle_path)

        self.assertEqual(res.actual["trade_count"], 3)
        # Strategy fires one market BUY; the simulator should match it
        # against a subsequent observed trade.
        self.assertGreaterEqual(res.actual["fill_count"], 1)
        self.assertEqual(res.expected["trade_count"], 3)


class BundleValidateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="flox-bundle-test-"))
        strat = _write_strategy(self.work)
        tape_dir = _write_tape(self.work)
        self.bundle_path = self.work / "bundle.tar"
        bundle.pack_bundle(
            strategy=strat,
            tape=tape_dir,
            output=self.bundle_path,
        )

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_fresh_bundle_validates_byte_equal(self) -> None:
        res = bundle.validate_bundle(self.bundle_path)
        self.assertTrue(
            res.matches,
            f"validate failed: diff={res.diff}, actual={res.actual}, "
            f"expected={res.expected}",
        )
        self.assertEqual(res.diff, [])

    def test_unsupported_bundle_format_rejected(self) -> None:
        # Rewrite the manifest with an impossible version, repack.
        broken = self.work / "broken.tar"
        with tarfile.open(self.bundle_path, "r") as src, \
             tarfile.open(broken, "w") as dst:
            for member in src.getmembers():
                f = src.extractfile(member)
                if member.name == "manifest.json":
                    m = json.loads(f.read())
                    m["bundle_format_version"] = 999
                    data = json.dumps(m).encode("utf-8")
                    info = tarfile.TarInfo("manifest.json")
                    info.size = len(data)
                    dst.addfile(info, fileobj=__import__("io").BytesIO(data))
                else:
                    dst.addfile(member, fileobj=f)
        with self.assertRaises(ValueError):
            bundle.validate_bundle(broken)


if __name__ == "__main__":
    unittest.main()
