#!/usr/bin/env python3
"""
scripts/gen_pyi_stubs.py

Regenerate `python/flox_py/__init__.pyi` and `python/flox_py/targets.pyi`
from the built `flox_py` extension via pybind11-stubgen.

Without committed stubs, type checkers (mypy, pyright) and AI agents
(Cursor, Claude Code, Copilot) treat the pybind11 module as opaque and
hallucinate method names. Committed stubs + a CI sync gate close that
gap; the generated `.pyi` files ship inside the wheel via
`python/flox_py/`, so PyPI users get types automatically (PEP 561 marker
is `python/flox_py/py.typed`).

The script imports `flox_py` from `build/python` (set by
`PYTHONPATH=build/python` if not already importable) — flox_py must be
built first, e.g. `cmake --build build --target _flox_py`.

Usage:
    python3 scripts/gen_pyi_stubs.py            # write
    python3 scripts/gen_pyi_stubs.py --check    # exit 1 if stubs drift
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PKG_SRC = REPO / "python" / "flox_py"
# Override with FLOX_PY_BUILD_DIR=path/to/python when the .so lives
# outside the default `build/python` (e.g. a per-Python-version build).
BUILD_PYTHON = Path(os.environ.get("FLOX_PY_BUILD_DIR", REPO / "build" / "python"))

# Stub layout produced by stubgen for our package:
#   flox_py/__init__.pyi               wrapper that re-exports symbols
#   flox_py/_flox_py/__init__.pyi      typed signatures of the C extension
#   flox_py/_flox_py/targets.pyi       typed signatures of the C submodule
# Even though `_flox_py` is a `.so` at runtime (not a directory), type
# checkers happily resolve `flox_py._flox_py.X` against
# `flox_py/_flox_py/__init__.pyi` when the parent package has `py.typed`.
STUB_FILES = (
    Path("__init__.pyi"),
    Path("_flox_py") / "__init__.pyi",
    Path("_flox_py") / "targets.pyi",
)

# `(anonymous namespace)::PyExtBar` shows up as a return-type annotation
# for `aggregate_*_bars` because PyExtBar is a translation-unit-local
# numpy dtype; stubgen replaces the whole annotation with `...` (i.e.
# Any), which is fine — the runtime return is a structured numpy array.
IGNORE_INVALID_RE = r"anonymous namespace"


def ensure_module_importable() -> None:
    if not (BUILD_PYTHON / "flox_py" / "__init__.py").exists():
        sys.exit(
            "error: flox_py package not found in build/python. Build it first:\n"
            "    cmake --build build --target _flox_py"
        )
    sys.path.insert(0, str(BUILD_PYTHON))
    try:
        __import__("flox_py")
    except ImportError as e:
        sys.exit(f"error: cannot import flox_py from {BUILD_PYTHON}: {e}")


def sort_import_lines(text: str) -> str:
    """Sort consecutive `from ... import ...` lines alphabetically.

    pybind11 module-attribute insertion order is not stable across
    platforms (macOS vs Linux) — `dir()` reflects that, and stubgen's
    output reflects `dir()`. Sorting the import block makes the stub
    deterministic so the CI sync gate doesn't fire on platform drift.
    """
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    block: list[str] = []

    def flush() -> None:
        if block:
            out.extend(sorted(block, key=str.lower))
            block.clear()

    for line in lines:
        if line.startswith("from ") and " import " in line:
            block.append(line)
        else:
            flush()
            out.append(line)
    flush()
    return "".join(out)


# `numpy.typing.NDArray[X]` annotations where `X` is either `...`
# (stubgen's placeholder for a type it could not resolve) or `PyExtBar`
# (a numpy structured-dtype struct that has no Python class) confuse
# mypy: it reports `Unexpected "..."` / undefined name and silently
# degrades the surrounding class to `Any`. Rewrite both shapes to a
# generic structured-array annotation that mypy parses cleanly.
_NDARRAY_VOID = "numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]"
_NDARRAY_REWRITES = (
    ("numpy.typing.NDArray[...]", _NDARRAY_VOID),
    ("numpy.typing.NDArray[PyExtBar]", _NDARRAY_VOID),
)


def fix_invalid_annotations(text: str) -> str:
    for old, new in _NDARRAY_REWRITES:
        text = text.replace(old, new)
    # pybind11 binds `__eq__(self, other: T)` literally as `T`, but
    # Python's data model requires `object` (Liskov). mypy flags this as
    # an override violation. Every dunder eq we've seen is a single-arg
    # comparator, so widening the parameter to `object` is safe.
    text = re.sub(
        r"def __eq__\(self, arg0: \w+\) -> bool:",
        "def __eq__(self, arg0: object) -> bool:",
        text,
    )
    return text


def post_process(text: str) -> str:
    return fix_invalid_annotations(sort_import_lines(text))


def run_stubgen(out_dir: Path) -> None:
    env = os.environ.copy()
    env["PYTHONPATH"] = f"{BUILD_PYTHON}{os.pathsep}{env.get('PYTHONPATH', '')}"
    cmd = [
        sys.executable,
        "-m",
        "pybind11_stubgen",
        "flox_py",
        "--output-dir",
        str(out_dir),
        "--ignore-invalid-expressions",
        IGNORE_INVALID_RE,
    ]
    # Don't pass --exit-code: it would fail even on the recoverable
    # per-symbol errors (ignored anonymous-namespace return types are
    # reduced to `...` in the stub, which is fine).
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.exit(f"error: pybind11-stubgen failed (rc={result.returncode})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if existing stubs differ from regenerated content.",
    )
    args = parser.parse_args()

    ensure_module_importable()

    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)
        run_stubgen(tmp_root)
        gen_pkg = tmp_root / "flox_py"

        for rel in STUB_FILES:
            src = gen_pkg / rel
            if not src.exists():
                sys.exit(f"error: stubgen did not produce {rel}")

        if args.check:
            ok = True
            for rel in STUB_FILES:
                committed = PKG_SRC / rel
                generated = post_process((gen_pkg / rel).read_text())
                current = committed.read_text() if committed.exists() else ""
                if current != generated:
                    print(
                        f"::error::{committed.relative_to(REPO)} is out of sync",
                        file=sys.stderr,
                    )
                    ok = False
            if not ok:
                print(
                    "Run: cmake --build build --target _flox_py "
                    "&& python3 scripts/gen_pyi_stubs.py",
                    file=sys.stderr,
                )
                return 1
            return 0

        for rel in STUB_FILES:
            dst = PKG_SRC / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(post_process((gen_pkg / rel).read_text()))
            print(
                f"wrote {dst.relative_to(REPO)} "
                f"({dst.stat().st_size:,} bytes)"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
