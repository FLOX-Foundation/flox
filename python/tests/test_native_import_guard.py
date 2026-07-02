"""Tests for the flox_py native-extension import guard.

`flox_py/__init__.py` refuses to import when `flox_py._flox_py`
resolves to the typing-stub directory (an empty namespace package)
instead of the compiled extension — the failure mode when the `.so`
was built for a different Python version. These tests reproduce that
layout in a scratch tree and assert the guard raises a clear
ImportError rather than letting attribute access fail later.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
INIT_SOURCE = REPO_ROOT / "python" / "flox_py" / "__init__.py"


def _import_from_synthetic_tree(with_stub_dir: bool) -> subprocess.CompletedProcess:
    """Build ``<tmp>/flox_py`` containing the real ``__init__.py`` and,
    optionally, a stub-only ``_flox_py/`` directory (no compiled
    extension either way), then ``import flox_py`` in a subprocess."""
    tmp = Path(tempfile.mkdtemp(prefix="flox-import-guard-"))
    pkg = tmp / "flox_py"
    pkg.mkdir()
    (pkg / "__init__.py").write_text(INIT_SOURCE.read_text())
    if with_stub_dir:
        stub = pkg / "_flox_py"
        stub.mkdir()
        (stub / "__init__.pyi").write_text("class SymbolRegistry: ...\n")
    # -S skips site processing so an editable install of flox_py in the
    # running interpreter's site-packages (a MetaPathFinder hook, which
    # outranks PYTHONPATH) cannot shadow the synthetic tree.
    return subprocess.run(
        [sys.executable, "-S", "-c", "import flox_py"],
        capture_output=True,
        text=True,
        cwd=tmp,
        env={"PYTHONPATH": str(tmp)},
    )


class NativeImportGuardTests(unittest.TestCase):
    def test_stub_only_layout_raises_clear_import_error(self) -> None:
        proc = _import_from_synthetic_tree(with_stub_dir=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("ImportError", proc.stderr)
        self.assertIn("different Python version", proc.stderr)
        # The guard must fire before any downstream AttributeError.
        self.assertNotIn("AttributeError", proc.stderr)

    def test_missing_extension_without_stubs_still_fails_loudly(self) -> None:
        proc = _import_from_synthetic_tree(with_stub_dir=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Error", proc.stderr)

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
