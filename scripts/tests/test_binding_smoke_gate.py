"""Acceptance tests for scripts/check_binding_smoke.py.

The smoke gate walks each binding by reflection and calls what it finds. It
could only ever do that for a binding it managed to import, and it treated a
failed import as nothing to report: the in-process probe printed a `fatal`
record and exited 0, the caller turned that into `python: SKIP (...)` and an
empty problem list, and `main()` exited 0 on an empty list. A wheel that did
not load and an addon that was never built both read as
"OK: no binding-level failures" -- the gate was loudest exactly when it saw
nothing at all.

A binding that cannot be imported is the largest binding-level failure there
is, so it has to be the loudest. These tests drive the two halves with a
binding that raises on import, and with a Node package whose entry point
throws, and require a non-zero exit that names the binding.

Seam: `--root`, plus `PYTHONPATH` for the Python half. The Python probe
already runs as a subprocess that inherits the environment, so a fake
`flox_py` earlier on the path is enough to stand in for a broken wheel; the
Node half resolves its package directory from the repository root, so it
needs `--root` to be pointed at a fixture.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import pytest

from conftest import run_smoke


def _env(**extra: str) -> dict[str, str]:
    env = dict(os.environ)
    env.update(extra)
    return env


def _write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def test_a_python_binding_that_cannot_be_imported_is_a_failure(tmp_path):
    """A working binding is green; the same run against a wheel that raises on
    import must be red and must name it."""
    good = tmp_path / "good"
    good.mkdir()
    (good / "flox_py.py").write_text(
        "class Ok:\n"
        "    def ping(self):\n"
        "        return 1\n")
    ok = run_smoke(["--python"], env=_env(PYTHONPATH=str(good)))
    assert ok.returncode == 0, ok.output

    broken = tmp_path / "broken"
    broken.mkdir()
    (broken / "flox_py.py").write_text(
        'raise ImportError("libflox_capi.so: cannot open shared object file")\n')
    r = run_smoke(["--python"], env=_env(PYTHONPATH=str(broken)))

    assert "SKIP" not in r.output, r.output
    assert "flox_py" in r.output
    assert "cannot open shared object file" in r.output
    assert r.returncode != 0, r.output


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_a_node_addon_that_cannot_be_loaded_is_a_failure(tmp_path):
    root = tmp_path / "tree"
    _write(root, "node/index.js",
           "module.exports = { Ok: function Ok() {} };\n")
    ok = run_smoke(["--node", "--root", str(root)])
    assert ok.returncode == 0, ok.output

    _write(root, "node/index.js",
           "throw new Error('Cannot find module \\'./prebuilds/darwin-arm64/flox_node.node\\'');\n")
    r = run_smoke(["--node", "--root", str(root)])

    assert "SKIP" not in r.output, r.output
    assert "node" in r.output
    assert "Cannot find module" in r.output
    assert r.returncode != 0, r.output


def test_a_binding_that_was_never_built_is_a_failure(tmp_path):
    """An absent entry point is the same defect as one that throws: nothing
    was exercised. It used to print `SKIP (addon not built)` and exit 0."""
    root = tmp_path / "empty"
    root.mkdir()
    r = run_smoke(["--node", "--root", str(root)])

    assert "unrecognized arguments" not in r.output, r.output
    assert "SKIP" not in r.output, r.output
    assert "index.js" in r.output, "the failure has to name what it could not load"
    assert r.returncode != 0, r.output
