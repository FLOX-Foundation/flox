"""Smoke tests for the ``flox new`` CLI scaffolder.

These tests exercise flox_py.cli end-to-end without the compiled
extension: the CLI is pure-Python and just walks bundled template
files. We assert that:

  * `flox templates` lists at least the `research` template.
  * `flox new <name>` creates a directory with main.py + requirements.txt.
  * Placeholder substitution actually replaces __PROJECT_NAME__.
  * Re-running into a non-empty destination errors out cleanly.
"""
from __future__ import annotations

import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

# Allow `python -m unittest python.tests.test_flox_new_cli` from repo root
# whether or not the build artefact is on PYTHONPATH.
HERE = Path(__file__).resolve().parent
PY_PKG = HERE.parent
sys.path.insert(0, str(PY_PKG))

from flox_py import cli  # noqa: E402


class FloxNewCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="flox-new-"))
        self.cwd = Path.cwd()
        os.chdir(self.tmp)

    def tearDown(self) -> None:
        os.chdir(self.cwd)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_templates_lists_research(self) -> None:
        names = cli._list_templates()
        self.assertIn("research", names,
                      f"expected 'research' in templates, got {names!r}")

    def test_new_creates_project(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out):
            rc = cli.main(["new", "alpha"])
        self.assertEqual(rc, 0, out.getvalue())
        proj = self.tmp / "alpha"
        self.assertTrue((proj / "main.py").exists())
        self.assertTrue((proj / "main.ipynb").exists())
        self.assertTrue((proj / "requirements.txt").exists())
        self.assertTrue((proj / "README.md").exists())
        # Bundled sample CSV must come along — the template's main.py
        # reads it as the default DATA_CSV path.
        sample = proj / "data" / "btcusdt_sample.csv"
        self.assertTrue(sample.exists(),
                        f"expected bundled sample CSV at {sample}")
        with sample.open() as f:
            header = f.readline().strip()
        self.assertEqual(header, "timestamp,open,high,low,close,volume")

    def test_notebook_parses_and_is_substituted(self) -> None:
        import json
        rc = cli.main(["new", "Hedge-Bot"])
        self.assertEqual(rc, 0)
        nb_path = self.tmp / "Hedge-Bot" / "main.ipynb"
        with nb_path.open() as f:
            nb = json.load(f)
        self.assertEqual(nb.get("nbformat"), 4)
        self.assertGreater(len(nb.get("cells", [])), 0)
        joined = "".join("".join(c.get("source", []))
                         for c in nb["cells"])
        self.assertIn("Hedge-Bot", joined)
        self.assertIn("hedge_bot_strategy", joined)
        self.assertIn("HEDGE_BOT_DATA", joined)
        self.assertNotIn("__PROJECT_NAME__", joined)
        self.assertNotIn("__PROJECT_SLUG__", joined)
        self.assertNotIn("__PROJECT_ENV__", joined)

    def test_placeholder_substitution(self) -> None:
        rc = cli.main(["new", "Hedge-Bot"])
        self.assertEqual(rc, 0)
        main_py = (self.tmp / "Hedge-Bot" / "main.py").read_text()
        self.assertIn("Hedge-Bot", main_py)
        self.assertIn("hedge_bot_strategy", main_py,
                      "slug substitution should produce hedge_bot_strategy")
        self.assertIn("HEDGE_BOT_DATA", main_py,
                      "env placeholder should produce HEDGE_BOT_DATA")
        self.assertNotIn("__PROJECT_NAME__", main_py)
        self.assertNotIn("__PROJECT_SLUG__", main_py)
        self.assertNotIn("__PROJECT_ENV__", main_py)

    def test_unknown_template_errors(self) -> None:
        err = io.StringIO()
        with redirect_stderr(err):
            rc = cli.main(["new", "x", "--template", "does-not-exist"])
        self.assertNotEqual(rc, 0)
        self.assertIn("unknown template", err.getvalue())

    def test_destination_must_be_empty(self) -> None:
        (self.tmp / "occupied").mkdir()
        (self.tmp / "occupied" / "stale.txt").write_text("hi")
        err = io.StringIO()
        with redirect_stderr(err):
            rc = cli.main(["new", "occupied"])
        self.assertNotEqual(rc, 0)
        self.assertIn("not empty", err.getvalue())

    def test_slug_helper(self) -> None:
        self.assertEqual(cli._slug("Hedge-Bot"), "hedge_bot")
        self.assertEqual(cli._slug("a strategy 2"), "a_strategy_2")
        self.assertEqual(cli._slug("___"), "project")


if __name__ == "__main__":
    unittest.main()
