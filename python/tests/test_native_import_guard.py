"""Tests for the flox_py native-extension import guard.

When `flox_py._flox_py` resolves to the typing-stub directory (an
empty namespace package) instead of the compiled extension — a
source-tree import, or a `.so` built for a different Python version —
the package must still import so pure-Python surfaces (flox_py.cli,
flox_py.lookahead) keep working, but any access to a native name must
raise an ImportError naming the actual cause rather than a bare
AttributeError. These tests reproduce the no-extension layout by
copying the pure-Python package into a scratch tree.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC_PKG = REPO_ROOT / "python" / "flox_py"


def _run_in_degraded_tree(code: str) -> subprocess.CompletedProcess:
    """Copy the pure-Python package (no compiled extension) into a
    scratch tree and run ``code`` there in a subprocess. -S keeps an
    editable install's meta-path hook (which outranks PYTHONPATH) from
    shadowing the scratch tree."""
    tmp = Path(tempfile.mkdtemp(prefix="flox-import-guard-"))
    shutil.copytree(
        SRC_PKG, tmp / "flox_py",
        ignore=shutil.ignore_patterns("*.so", "*.pyd", "__pycache__"),
    )
    return subprocess.run(
        [sys.executable, "-S", "-c", code],
        capture_output=True,
        text=True,
        cwd=tmp,
        env={"PYTHONPATH": str(tmp)},
    )


class NativeImportGuardTests(unittest.TestCase):
    def test_package_imports_without_native_extension(self) -> None:
        proc = _run_in_degraded_tree("import flox_py")
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_pure_python_submodules_work_without_native(self) -> None:
        proc = _run_in_degraded_tree(
            "from flox_py import cli, lookahead; "
            "r = lookahead.analyze_source('df.shift(-1)'); "
            "assert not r.ok"
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_native_attribute_access_raises_clear_import_error(self) -> None:
        proc = _run_in_degraded_tree(
            "import flox_py; flox_py.SymbolRegistry"
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("ImportError", proc.stderr)
        self.assertIn("different Python version", proc.stderr)
        self.assertIn("flox_py.SymbolRegistry", proc.stderr)
        # The guard must preempt the bare-AttributeError failure mode.
        self.assertNotIn("AttributeError", proc.stderr)

    def test_real_package_imports_when_extension_matches(self) -> None:
        for cand in ("build/python", "build-py312/python"):
            p = REPO_ROOT / cand
            if p.is_dir():
                proc = subprocess.run(
                    [sys.executable, "-c",
                     "import flox_py; flox_py.SymbolRegistry"],
                    capture_output=True,
                    text=True,
                    env={"PYTHONPATH": str(p)},
                )
                if proc.returncode == 0:
                    return
        self.skipTest("no built extension matches this interpreter")


if __name__ == "__main__":
    unittest.main()
