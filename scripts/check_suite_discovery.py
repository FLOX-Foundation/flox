#!/usr/bin/env python3
"""Every test file must be reachable by the suite runner CI uses.

C++ has this guarantee in two parts: ctest runs whatever is registered, and
check_test_gating.py's reverse pass fails when a tests/*.cpp file is not
named by any add_flox_test(...) or manual add_executable(...) call in
tests/CMakeLists.txt. That guarantee is scoped to tests/ itself --
venue/tests/ and connectors/tests/ use file(GLOB ...) instead of a name
list, so nothing can be "forgotten" there by construction, but only once
the surrounding CMakeLists.txt actually configures that directory in a CI
job (connectors/tests/ was not configured in any workflow until FLOX_
BUILD_TESTS was added alongside FLOX_BUILD_CONNECTORS -- a prior version of
this docstring's "twice over" claim did not hold for either half).

Python, Node, and mcp/ had none of this. Because ci.yml listed test files
one per named step, a file nobody remembered to add simply never ran -- 34
of 69 Python files and 9 of 21 Node files at the time this check was
written, including every regression gate written for the binding-surface
audit. The tests existed in the repo and protected nothing.

CI now runs the suites wholesale. This script is the other half: it fails when
a test file exists that the suite runner would not pick up, so the failure mode
cannot come back in a different shape.

What "reachable" means per language:

  Python  pytest's default discovery from python/tests: files named test_*.py
          holding at least one test_* function or Test* class. A file with
          neither is dead weight pytest silently collects nothing from.
  mcp     same rule as Python, applied to mcp/tests -- run via
          `pytest mcp/tests/` in ci.yml, not the python/tests suite.
  Node    the `for f in test/test_*.js` loop, i.e. the test_ prefix. Helpers
          that are not tests must not use that prefix.

Run:
    python3 scripts/check_suite_discovery.py
    python3 scripts/check_suite_discovery.py --verbose
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PY_TESTS = REPO / "python" / "tests"
MCP_TESTS = REPO / "mcp" / "tests"
NODE_TESTS = REPO / "node" / "test"
CI = REPO / ".github" / "workflows" / "ci.yml"

# Helpers that legitimately live beside the tests without being tests. Keep
# this list short and justified -- it is the escape hatch, not the norm.
NODE_NON_TEST = {"byte_identical_fixture.js"}


def _pytest_dir_problems(test_dir: Path, *, label: str, verbose: bool) -> list[str]:
    """Shared pytest-discovery check for python/tests and mcp/tests."""
    out: list[str] = []
    if not test_dir.is_dir():
        return [f"{test_dir} does not exist"]

    for path in sorted(test_dir.glob("*.py")):
        if path.name in ("conftest.py", "__init__.py"):
            continue
        rel = path.relative_to(REPO)
        if not path.name.startswith("test_"):
            out.append(
                f"{rel}: lives in {label} but is not named test_*.py, so "
                f"pytest will not collect it. Rename it, or move it out of the "
                f"test directory if it is a helper."
            )
            continue
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"))
        except SyntaxError as exc:
            out.append(f"{rel}: does not parse ({exc})")
            continue
        has_case = any(
            (isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
             and n.name.startswith("test_"))
            or (isinstance(n, ast.ClassDef)
                and (n.name.startswith("Test")
                     or any(getattr(b, "attr", getattr(b, "id", "")) == "TestCase"
                            for b in n.bases)))
            for n in ast.walk(tree)
        )
        if not has_case:
            out.append(
                f"{rel}: named test_*.py but holds no test_* function and no "
                f"TestCase/Test* class, so pytest collects nothing from it. "
                f"Either add cases or delete the file."
            )
        elif verbose:
            print(f"  ok     {rel}")
    return out


def python_problems(verbose: bool) -> list[str]:
    return _pytest_dir_problems(PY_TESTS, label="python/tests", verbose=verbose)


def mcp_problems(verbose: bool) -> list[str]:
    return _pytest_dir_problems(MCP_TESTS, label="mcp/tests", verbose=verbose)


def node_problems(verbose: bool) -> list[str]:
    out: list[str] = []
    if not NODE_TESTS.is_dir():
        return [f"{NODE_TESTS} does not exist"]

    for path in sorted(NODE_TESTS.glob("*.js")):
        rel = path.relative_to(REPO)
        if path.name in NODE_NON_TEST:
            if verbose:
                print(f"  helper {rel}")
            continue
        if not path.name.startswith("test_"):
            out.append(
                f"{rel}: the CI loop globs test/test_*.js, so this file never "
                f"runs. Rename it to test_*.js, or add it to NODE_NON_TEST in "
                f"{Path(__file__).name} with a reason if it is a helper."
            )
        elif verbose:
            print(f"  ok     {rel}")
    return out


def ci_problems() -> list[str]:
    """Guard the runners themselves, so nobody re-lists files by hand."""
    out: list[str] = []
    if not CI.is_file():
        return [f"{CI} does not exist"]
    text = CI.read_text(encoding="utf-8")

    if "pytest python/tests" not in text:
        out.append(
            "ci.yml no longer runs `pytest python/tests` -- the Python suite is "
            "not being run wholesale."
        )
    if "pytest mcp/tests" not in text:
        out.append(
            "ci.yml no longer runs `pytest mcp/tests` -- the mcp/ suite is not "
            "being run wholesale."
        )
    if "for f in test/test_*.js" not in text:
        out.append(
            "ci.yml no longer loops over test/test_*.js -- the Node suite is not "
            "being run wholesale."
        )

    # Re-listing individual files is the exact regression this guards against.
    named_py = re.findall(r"python3 python/tests/test_\w+\.py", text)
    if named_py:
        out.append(
            f"ci.yml invokes {len(named_py)} Python test file(s) by name "
            f"({named_py[0]} ...). The suite already runs them; naming files "
            f"individually is how half the suite stopped running."
        )
    named_node = re.findall(r"node test/test_\w+\.js", text)
    if named_node:
        out.append(
            f"ci.yml invokes {len(named_node)} Node test file(s) by name "
            f"({named_node[0]} ...). Use the suite loop."
        )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    problems = (
        python_problems(args.verbose)
        + mcp_problems(args.verbose)
        + node_problems(args.verbose)
        + ci_problems()
    )

    n_py = len(list(PY_TESTS.glob("test_*.py")))
    n_mcp = len(list(MCP_TESTS.glob("test_*.py")))
    n_nd = len([p for p in NODE_TESTS.glob("test_*.js")])
    print(f"Checked {n_py} Python, {n_mcp} mcp, and {n_nd} Node test files "
          f"for suite reachability.")

    if problems:
        print("\nerror: test files or runners that the suite would not cover:\n",
              file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("OK: every test file is reachable by the suite CI runs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
