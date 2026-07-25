"""Scaffolded templates must actually trade when run.

test_flox_new_cli.py deliberately runs without the compiled extension, so it
asserts on file existence and placeholder substitution only. That left the
generated code itself unverified, and two of the three templates shipped with
an exit branch that could never fire:

    if fv > sv and ctx.is_flat():
        self.market_buy(0.01)
    elif fv < sv and ctx.is_flat():     # after the entry, never flat again
        self.market_sell(0.01)

The position opened once and was never closed, so `total_trades` -- which
counts closed round trips -- stayed at zero and every printed metric read
0.0000. The first thing a new user ran made the library look inert, and no
test noticed because "the file exists and contains the project name" passes
whether or not the strategy works.

These tests execute the scaffold the way a user does.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PY_PKG = HERE.parent

# Deliberately NOT putting PY_PKG on sys.path: the source tree's flox_py has
# no compiled extension, so importing it here would shadow the installed
# package and skip every test in this file. PY_PKG is used as a filesystem
# path only. The subprocesses below run from a temp dir and resolve the
# installed flox_py, which is what a user has.
try:
    import flox_py  # noqa: F401

    flox_py.SymbolRegistry()
    _HAVE_EXT = True
except Exception:  # pragma: no cover - depends on the local build
    _HAVE_EXT = False


def _trade_count(stdout: str) -> int:
    """Pull N out of the scaffold's `  trades : N  win=..%` line."""
    line = next((ln for ln in stdout.splitlines() if "trades :" in ln), None)
    assert line is not None, f"no trade-count line in output:\n{stdout}"
    return int(line.split("trades :")[1].split()[0])


def _scaffold(tmp: str, name: str, template: str | None = None) -> Path:
    argv = ["-m", "flox_py.cli", "new", name]
    if template:
        argv += ["--template", template]
    r = subprocess.run([sys.executable, *argv], cwd=tmp, capture_output=True, text=True)
    assert r.returncode == 0, f"flox new failed: {r.stderr}"
    return Path(tmp) / name


@unittest.skipUnless(_HAVE_EXT, "needs the compiled flox_py extension")
class ResearchTemplateRuns(unittest.TestCase):
    def test_scaffolded_project_runs_and_trades(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proj = _scaffold(tmp, "probe")
            r = subprocess.run([sys.executable, "main.py"], cwd=proj,
                               capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stderr)

            self.assertGreater(
                _trade_count(r.stdout), 0,
                "the default scaffold closed no trades -- it looks inert to a "
                f"new user. Output was:\n{r.stdout}")

    def test_scaffolded_project_writes_a_non_empty_report(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proj = _scaffold(tmp, "probe")
            subprocess.run([sys.executable, "main.py"], cwd=proj,
                           capture_output=True, text=True, timeout=300)
            report = proj / "report.html"
            self.assertTrue(report.exists(), "no report.html was written")
            html = report.read_text()
            # An all-zero run still renders a page, so assert on the trades
            # table having at least one row rather than on the file existing.
            self.assertIn("<tr", html)


@unittest.skipUnless(_HAVE_EXT, "needs the compiled flox_py extension")
class IndicatorLibraryTemplateRuns(unittest.TestCase):
    def test_strategy_example_trades(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proj = _scaffold(tmp, "indlib", template="indicator-library")
            example = proj / "examples" / "use_in_strategy.py"
            self.assertTrue(example.exists())

            # The README's flow is `pip install -e .` then
            # `python examples/use_in_strategy.py` from the project root, so
            # the scaffolded package must be importable. PYTHONPATH stands in
            # for the editable install.
            env = dict(os.environ, PYTHONPATH=str(proj))
            r = subprocess.run([sys.executable, "examples/use_in_strategy.py"],
                               cwd=proj, env=env,
                               capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stderr)

            self.assertGreater(_trade_count(r.stdout), 0,
                               f"the example closed no trades:\n{r.stdout}")


class EveryTemplateHasAReachableExit(unittest.TestCase):
    """Static guard, so this holds even where the extension is unavailable.

    An entry gated on is_flat() paired with an exit also gated on is_flat()
    is the exact shape of the bug: the second branch is dead code.
    """

    def test_no_template_gates_both_branches_on_is_flat(self) -> None:
        offenders = []
        for path in (PY_PKG / "flox_py" / "templates").rglob("*"):
            if path.suffix not in {".py", ".ipynb", ".md"} or not path.is_file():
                continue
            lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
            for i, line in enumerate(lines):
                stripped = line.strip().lstrip('"').strip()
                # Scanning line by line, not splitting on "if " -- "elif "
                # contains "if ", so a naive split tears the branch apart and
                # the guard silently never fires.
                if not (stripped.startswith("if ") and "ctx.is_flat()" in stripped):
                    continue
                for follow in lines[i + 1:i + 5]:
                    f = follow.strip().lstrip('"').strip()
                    if f.startswith("elif ") and "ctx.is_flat()" in f:
                        offenders.append(str(path.relative_to(PY_PKG)))
                        break
                if offenders and offenders[-1].endswith(path.name):
                    break
        self.assertEqual(
            sorted(set(offenders)), [],
            "template(s) pair an is_flat() entry with an is_flat() exit; the "
            "exit branch can never run: " + ", ".join(sorted(set(offenders))))


if __name__ == "__main__":
    unittest.main()
