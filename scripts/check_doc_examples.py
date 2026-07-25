#!/usr/bin/env python3
"""scripts/check_doc_examples.py

Run every executable doc example so the `--8<--` include pattern is
load-bearing: docs that inline a real file under `docs/examples/` go red the
moment that file stops working.

What it does:

* `docs/examples/*.py` — execute with this interpreter, fail on non-zero
  exit. Entries in `SKIP_PY` are skipped with a printed reason.
* `docs/examples/*.js` — `node --check` (syntax only; running them needs the
  built NAPI addon). Skipped with a notice when `node` is not on PATH.

The Python extension `flox_py` is a compiled artifact. When it is not
importable (a docs-only CI job, or a fresh checkout with no build), the
Python examples are **syntax-checked** instead of executed and the script
says so. Pass `--require-runtime` in a job that has built the bindings to
turn "cannot import flox_py" into a failure — that is where the real
execution coverage comes from.

`build/python` is prepended to `PYTHONPATH` automatically when it exists, so
a local `cmake --build build` is enough to get full execution.

Usage:
    python3 scripts/check_doc_examples.py
    python3 scripts/check_doc_examples.py --require-runtime   # CI w/ build
    python3 scripts/check_doc_examples.py --quiet
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import Dict, List


REPO_ROOT = Path(__file__).resolve().parents[1]
EXAMPLES_DIR = REPO_ROOT / "docs" / "examples"

# Examples that cannot run in CI. Every entry needs a reason; anything
# runnable belongs in the executed set, not here.
SKIP_PY: Dict[str, str] = {
    "python_ccxt_live.py":
        "opens a live websocket via ccxt.pro — needs network and, for the "
        "private streams, exchange credentials",
}

SKIP_JS: Dict[str, str] = {}

# Per-example wall clock. Examples are backtests over small fixtures; a
# minute is already pathological and means the example hung.
TIMEOUT_S = 120


def _child_env() -> Dict[str, str]:
    env = os.environ.copy()
    build_python = REPO_ROOT / "build" / "python"
    if build_python.is_dir():
        existing = env.get("PYTHONPATH", "")
        parts = [str(build_python)] + ([existing] if existing else [])
        env["PYTHONPATH"] = os.pathsep.join(parts)
    return env


def _runtime_available(env: Dict[str, str]) -> bool:
    """True when the *compiled* extension is usable, not just importable.

    `flox_py/__init__.py` imports fine from a source tree and only raises on
    first attribute access, so the probe touches a native symbol.
    """
    probe = subprocess.run(
        [sys.executable, "-c", "import flox_py; flox_py.SymbolRegistry"],
        cwd=REPO_ROOT, env=env, capture_output=True, text=True,
    )
    return probe.returncode == 0


def _fail(path: Path, message: str, detail: str = "") -> str:
    rel = path.relative_to(REPO_ROOT).as_posix()
    out = f"::error file={rel}::{message}"
    if detail:
        indented = "\n".join(f"    {line}" for line in
                             detail.strip().splitlines()[-25:])
        out += "\n" + indented
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Execute docs/examples/*.py and syntax-check "
                    "docs/examples/*.js."
    )
    parser.add_argument("--require-runtime", action="store_true",
                        help="fail if flox_py cannot be imported instead of "
                             "degrading to a syntax check")
    parser.add_argument("--quiet", action="store_true",
                        help="only print failures")
    args = parser.parse_args()

    if not EXAMPLES_DIR.is_dir():
        print(f"::error::{EXAMPLES_DIR} not found", file=sys.stderr)
        return 1

    env = _child_env()
    failures: List[str] = []
    notices: List[str] = []

    py_files = sorted(EXAMPLES_DIR.glob("*.py"))
    js_files = sorted(EXAMPLES_DIR.glob("*.js"))

    runtime = _runtime_available(env)
    if not runtime:
        if args.require_runtime:
            print("::error::flox_py is not importable, but "
                  "--require-runtime was passed. Build the Python bindings "
                  "(-DFLOX_BUILD_PYTHON=ON) or set PYTHONPATH.",
                  file=sys.stderr)
            return 1
        notices.append(
            "::notice::flox_py is not importable — Python examples are "
            "syntax-checked only. Run with a built extension "
            "(PYTHONPATH=build/python) for full execution coverage."
        )

    executed = skipped = syntax_only = 0

    for path in py_files:
        if path.name in SKIP_PY:
            skipped += 1
            if not args.quiet:
                print(f"SKIP {path.name}: {SKIP_PY[path.name]}")
            continue

        if not runtime:
            syntax_only += 1
            source = path.read_text(encoding="utf-8")
            try:
                compile(source, str(path), "exec")
            except SyntaxError as exc:
                failures.append(_fail(
                    path, f"syntax error at line {exc.lineno}: {exc.msg}"))
            continue

        try:
            proc = subprocess.run(
                [sys.executable, str(path)],
                cwd=REPO_ROOT, env=env, capture_output=True, text=True,
                timeout=TIMEOUT_S,
            )
        except subprocess.TimeoutExpired:
            failures.append(_fail(
                path, f"timed out after {TIMEOUT_S}s — a doc example must "
                      f"terminate on its own"))
            continue

        executed += 1
        if proc.returncode != 0:
            failures.append(_fail(
                path,
                f"exited {proc.returncode} — the example this page includes "
                f"via `--8<--` no longer runs",
                proc.stderr or proc.stdout,
            ))

    node = _which("node")
    checked_js = 0
    if node is None:
        notices.append("::notice::node not on PATH — skipped `node --check` "
                       "for docs/examples/*.js")
    else:
        for path in js_files:
            if path.name in SKIP_JS:
                if not args.quiet:
                    print(f"SKIP {path.name}: {SKIP_JS[path.name]}")
                continue
            proc = subprocess.run(
                [node, "--check", str(path)],
                cwd=REPO_ROOT, capture_output=True, text=True,
                timeout=TIMEOUT_S,
            )
            checked_js += 1
            if proc.returncode != 0:
                failures.append(_fail(
                    path, "node --check failed", proc.stderr or proc.stdout))

    rc = 0
    for notice in notices:
        print(notice)
    if failures:
        rc = 1
        print(f"::error::{len(failures)} doc example(s) broken:",
              file=sys.stderr)
        for line in failures:
            print(line, file=sys.stderr)

    if not args.quiet:
        print(f"Python examples: {executed} executed, {syntax_only} "
              f"syntax-checked, {skipped} skipped "
              f"({len(py_files)} total). JS examples: {checked_js} "
              f"node --check'ed ({len(js_files)} total).")
        if rc == 0:
            print("OK: every doc example is healthy.")
    return rc


def _which(binary: str) -> str | None:
    from shutil import which
    return which(binary)


if __name__ == "__main__":
    sys.exit(main())
