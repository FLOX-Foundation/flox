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

    def test_templates_lists_live(self) -> None:
        names = cli._list_templates()
        self.assertIn("live", names,
                      f"expected 'live' template, got {names!r}")

    def test_live_template_layout(self) -> None:
        rc = cli.main(["new", "MyBot", "--template", "live"])
        self.assertEqual(rc, 0)
        proj = self.tmp / "MyBot"
        for f in ("main.py", "config.py", "config.toml.example",
                  "requirements.txt", "README.md", ".gitignore"):
            self.assertTrue((proj / f).exists(),
                            f"live template should ship {f}")
        # Ensure the example config does NOT carry secrets and that
        # the scaffolded project's actual config.toml is absent (only
        # the example is in git; the runtime file is created by the
        # user from the example).
        self.assertFalse((proj / "config.toml").exists())

    def test_live_template_substitutes_prefix_without_data_suffix(self) -> None:
        rc = cli.main(["new", "Live-Bot", "--template", "live"])
        self.assertEqual(rc, 0)
        cfg = (self.tmp / "Live-Bot" / "config.py").read_text()
        # Prefix is upper-cased slug WITHOUT the "_DATA" suffix.
        self.assertIn("LIVE_BOT", cfg)
        self.assertNotIn("LIVE_BOT_DATA", cfg)
        self.assertNotIn("__PROJECT_NAME__", cfg)
        self.assertNotIn("__PROJECT_PREFIX__", cfg)
        self.assertNotIn("__PROJECT_ENV__", cfg)
        self.assertNotIn("__PROJECT_SLUG__", cfg)

    # ── indicator-library template ────────────────────────────────

    def test_templates_lists_indicator_library(self) -> None:
        names = cli._list_templates()
        self.assertIn("indicator-library", names,
                      f"expected 'indicator-library', got {names!r}")

    def test_indicator_library_layout(self) -> None:
        rc = cli.main(["new", "my-indicators",
                       "--template", "indicator-library"])
        self.assertEqual(rc, 0)
        proj = self.tmp / "my-indicators"
        for f in ("pyproject.toml", "README.md", ".gitignore"):
            self.assertTrue((proj / f).exists(),
                            f"indicator-library should ship {f}")
        # Package directory was named __PROJECT_SLUG__ in the template;
        # path substitution must rename it to my_indicators/.
        pkg = proj / "my_indicators"
        self.assertTrue(pkg.is_dir(),
                        f"package dir should be {pkg}, listing: "
                        f"{sorted(p.name for p in proj.iterdir())}")
        self.assertTrue((pkg / "__init__.py").exists())
        self.assertTrue((pkg / "zlema.py").exists())
        # tests with bundled CSV.
        sample = proj / "tests" / "data" / "btcusdt_sample.csv"
        self.assertTrue(sample.exists())
        with sample.open() as f:
            self.assertEqual(f.readline().strip(),
                             "timestamp,open,high,low,close,volume")
        self.assertTrue((proj / "tests" / "test_zlema.py").exists())
        self.assertTrue((proj / "examples" / "use_in_strategy.py").exists())
        self.assertTrue(
            (proj / ".github" / "workflows" / "ci.yml").exists())

    def test_indicator_library_substitutions_in_paths_and_content(self) -> None:
        rc = cli.main(["new", "Hedge-Indicators",
                       "--template", "indicator-library"])
        self.assertEqual(rc, 0)
        proj = self.tmp / "Hedge-Indicators"
        # Path substitution: package directory uses the snake-case slug.
        self.assertTrue((proj / "hedge_indicators" / "__init__.py").exists())
        # No leftover placeholders anywhere.
        for p in proj.rglob("*"):
            if p.is_file():
                self.assertNotIn("__PROJECT_NAME__", p.name)
                self.assertNotIn("__PROJECT_SLUG__", p.name)
                try:
                    txt = p.read_text()
                except UnicodeDecodeError:
                    continue
                self.assertNotIn("__PROJECT_NAME__", txt,
                                 f"unsubstituted name in {p}")
                self.assertNotIn("__PROJECT_SLUG__", txt,
                                 f"unsubstituted slug in {p}")
                self.assertNotIn("__PROJECT_PREFIX__", txt,
                                 f"unsubstituted prefix in {p}")
                self.assertNotIn("__PROJECT_ENV__", txt,
                                 f"unsubstituted env in {p}")
        # pyproject.toml carries the slug as the package name and as
        # an entry-point target.
        toml = (proj / "pyproject.toml").read_text()
        self.assertIn('name = "hedge_indicators"', toml)
        self.assertIn("hedge_indicators.zlema:ZLEMA", toml)
        # README mentions the project & slug forms.
        readme = (proj / "README.md").read_text()
        self.assertIn("Hedge-Indicators", readme)
        self.assertIn("hedge_indicators", readme)

    def test_indicator_library_zlema_module_imports_and_runs(self) -> None:
        """Scaffolded ZLEMA must be import-and-run-able as standalone code."""
        import importlib.util

        rc = cli.main(["new", "my-indicators",
                       "--template", "indicator-library"])
        self.assertEqual(rc, 0)
        zlema_path = self.tmp / "my-indicators" / "my_indicators" / "zlema.py"
        spec = importlib.util.spec_from_file_location(
            "_scaffolded_zlema", zlema_path)
        assert spec is not None and spec.loader is not None
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        z = mod.ZLEMA(period=10)
        self.assertFalse(z.ready)
        # Feed enough samples to clear warmup (period + lag).
        for price in [100.0 + i * 0.5 for i in range(40)]:
            z.update(price)
        self.assertTrue(z.ready)
        self.assertIsNotNone(z.value)


if __name__ == "__main__":
    unittest.main()
