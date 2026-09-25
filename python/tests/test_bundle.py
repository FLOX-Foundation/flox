"""Tests for the reproducibility bundle."""
from __future__ import annotations

import io
import json
import shutil
import sys
import tarfile
import tempfile
import unittest
import warnings
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


_TAKE_PROFIT_STRATEGY_SOURCE = '''
"""Hangs one take-profit above the market on the first trade."""
import flox_py as flox


class TakeProfitOnFirstTrade(flox.Strategy):
    def __init__(self, symbols, qty=1.0):
        super().__init__(symbols)
        self.qty = qty
        self._fired = False

    def on_trade(self, ctx, trade):
        if self._fired:
            return
        self._fired = True
        self.take_profit_market(side="sell", trigger=100.75, qty=self.qty)
'''


class BundleConditionalOrderTests(unittest.TestCase):
    """A bundled strategy's conditional-order signals have to reach the book.

    The replay path turns each Signal into a SimulatedExecutor.submit_order
    call, and both sides now spell the order types the same way. A
    translation layer between them -- the one this fix removed -- is
    invisible to every other bundle test, because every other bundled
    strategy only ever calls market_buy(): the names it would rewrite
    never appear. submit_order refuses an unknown name outright now, so a
    reintroduced translation does not mis-route the order, it loses it.
    """

    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="flox-bundle-tp-test-"))
        self.strat = self.work / "strategy.py"
        self.strat.write_text(_TAKE_PROFIT_STRATEGY_SOURCE)
        self.tape_dir = _write_tape(self.work)

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_a_take_profit_signal_reaches_the_executor(self) -> None:
        res = bundle._run_strategy_against_tape(self.strat, self.tape_dir)

        self.assertEqual(res["trade_count"], 3)
        # The tape prints 100.00, 100.50, 101.00; the take-profit arms at
        # 100.75 on the first of them and fires on the last, booking its
        # own trigger rather than the print that crossed it.
        self.assertEqual(res["fill_count"], 1, res)
        fill = res["fills"][0]
        self.assertEqual(fill["side"], "sell")
        self.assertEqual(fill["price"], 100.75)
        self.assertEqual(fill["quantity"], 1.0)

    def test_the_same_signal_survives_a_pack_and_replay(self) -> None:
        out = self.work / "bundle.tar"
        bundle.pack_bundle(strategy=self.strat, tape=self.tape_dir, output=out)
        res = bundle.replay_bundle(out)

        self.assertEqual(res.actual["fill_count"], 1, res.actual)
        self.assertEqual(res.actual["fills"], res.expected["fills"])


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
        # Every fresh bundle now also ships an
        # expected.floxrun directory alongside the legacy JSON.
        self.assertTrue(
            any(n.startswith("expected.floxrun/") or n == "expected.floxrun"
                for n in names),
            f"missing expected.floxrun in bundle entries: {sorted(names)}",
        )

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


def _repack_with_member(bundle_path: Path, dest: Path, extra_members: dict) -> None:
    """Copy `bundle_path` to `dest`, adding each (name -> bytes) in
    `extra_members` as an additional tar member (used to smuggle a
    path-traversal entry into an otherwise normal bundle)."""
    with tarfile.open(bundle_path, "r") as src, tarfile.open(dest, "w") as dst:
        for member in src.getmembers():
            f = src.extractfile(member)
            dst.addfile(member, fileobj=f)
        for name, data in extra_members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            dst.addfile(info, fileobj=io.BytesIO(data))


def _repack_replacing_member(bundle_path: Path, dest: Path, replacements: dict) -> None:
    """Copy `bundle_path` to `dest`, replacing the content of each
    member named in `replacements` (name -> bytes) and leaving every
    other member untouched -- used to simulate a bundle whose
    strategy.py was edited after packing without touching the
    manifest's recorded hashes."""
    with tarfile.open(bundle_path, "r") as src, tarfile.open(dest, "w") as dst:
        for member in src.getmembers():
            if member.name in replacements:
                data = replacements[member.name]
                info = tarfile.TarInfo(member.name)
                info.size = len(data)
                dst.addfile(info, fileobj=io.BytesIO(data))
            else:
                f = src.extractfile(member)
                dst.addfile(member, fileobj=f)


class BundleSecurityTests(unittest.TestCase):
    """Extraction must not escape the destination directory,
    and replaying/validating a bundle must warn that it executes
    the strategy code inside it."""

    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="flox-bundle-test-"))
        strat = _write_strategy(self.work)
        tape_dir = _write_tape(self.work)
        self.bundle_path = self.work / "bundle.tar"
        bundle.pack_bundle(strategy=strat, tape=tape_dir, output=self.bundle_path)

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_path_traversal_member_is_refused(self) -> None:
        evil = self.work / "evil.tar"
        _repack_with_member(
            self.bundle_path, evil,
            {"../escaped_from_bundle.txt": b"should never land outside the bundle root"},
        )
        with self.assertRaises(bundle.BundleSecurityError):
            bundle.replay_bundle(evil)
        # And nothing was written outside the (nonexistent, since
        # extraction should have been refused) extraction directory.
        self.assertFalse((self.work.parent / "escaped_from_bundle.txt").exists())

    def test_replay_warns_about_executing_bundle_code(self) -> None:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            bundle.replay_bundle(self.bundle_path)
        self.assertTrue(
            any(issubclass(w.category, bundle.BundleTrustWarning) for w in caught),
            f"expected a BundleTrustWarning, got {[w.category for w in caught]}",
        )


class BundleIntegrityTests(unittest.TestCase):
    """The manifest's recorded strategy/tape hashes and
    engine version must actually be checked, and a bundle predating
    the .floxrun ride-along must not be flagged as mismatched purely
    because of that."""

    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="flox-bundle-test-"))
        self.strat = _write_strategy(self.work)
        self.tape_dir = _write_tape(self.work)
        self.bundle_path = self.work / "bundle.tar"
        bundle.pack_bundle(strategy=self.strat, tape=self.tape_dir, output=self.bundle_path)

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_swapped_strategy_with_untouched_hash_is_detected(self) -> None:
        # The report's exact scenario: an attacker swaps strategy.py
        # for different code AND re-records expected_output.json to
        # match the new code's own output (so the JSON-level diff
        # alone shows nothing), but leaves the manifest's
        # strategy_sha256 as it was at pack time. Build that bundle by
        # packing the tampered strategy fresh (self-consistent
        # manifest + expected output for the new code), then splicing
        # its strategy.py and expected_output.json onto the ORIGINAL
        # bundle's manifest -- which still names the original hash.
        tampered_dir = Path(tempfile.mkdtemp(prefix="flox-bundle-tampered-"))
        try:
            tampered_source = _STRATEGY_SOURCE.replace("self.qty = qty", "self.qty = qty * 5")
            tampered_strat = tampered_dir / "strategy.py"
            tampered_strat.write_text(tampered_source)
            tampered_bundle = tampered_dir / "tampered.tar"
            bundle.pack_bundle(strategy=tampered_strat, tape=self.tape_dir, output=tampered_bundle)

            with tarfile.open(tampered_bundle, "r") as tf:
                new_strategy_bytes = tf.extractfile("strategy/strategy.py").read()
                new_expected_bytes = tf.extractfile("expected_output.json").read()

            spliced = self.work / "spliced.tar"
            _repack_replacing_member(
                self.bundle_path, spliced,
                {
                    "strategy/strategy.py": new_strategy_bytes,
                    "expected_output.json": new_expected_bytes,
                },
            )
            res = bundle.validate_bundle(spliced)
            self.assertFalse(
                res.matches,
                "a strategy swapped for different code, with a matching "
                "expected_output.json but the original manifest hash "
                "left untouched, must not validate as a match",
            )
            self.assertTrue(
                any(d.startswith("strategy_sha256:") for d in res.diff), res.diff
            )
        finally:
            shutil.rmtree(tampered_dir, ignore_errors=True)

    def test_faked_engine_version_is_detected(self) -> None:
        with tarfile.open(self.bundle_path, "r") as tf:
            manifest = json.loads(tf.extractfile("manifest.json").read())
        manifest["flox_version"] = "0.0.1-attacker"
        replaced = self.work / "fakeversion.tar"
        _repack_replacing_member(
            self.bundle_path, replaced,
            {"manifest.json": json.dumps(manifest).encode("utf-8")},
        )
        res = bundle.validate_bundle(replaced)
        self.assertTrue(any(d.startswith("flox_version:") for d in res.diff), res.diff)

    def test_bundle_without_floxrun_still_validates_on_matching_content(self) -> None:
        # Simulate a bundle packed before the .floxrun ride-along
        # landed: strip expected.floxrun/ entirely. Every other field
        # still matches byte-for-byte, so this must still validate --
        # comparing "actual has a fresh trace" against "expected never
        # had one" must not, on its own, fail the whole bundle.
        with tarfile.open(self.bundle_path, "r") as src:
            members = [m for m in src.getmembers() if not m.name.startswith("expected.floxrun")]
            old_format = self.work / "old_format.tar"
            with tarfile.open(old_format, "w") as dst:
                for m in members:
                    dst.addfile(m, fileobj=src.extractfile(m))
        res = bundle.validate_bundle(old_format)
        self.assertTrue(
            res.matches,
            f"a bundle with no expected.floxrun/ but otherwise-matching "
            f"content must still validate: diff={res.diff}",
        )
        self.assertNotIn("floxrun_present", res.actual)
        self.assertNotIn("floxrun_present", res.expected)


if __name__ == "__main__":
    unittest.main()
