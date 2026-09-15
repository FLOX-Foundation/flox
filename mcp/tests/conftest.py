"""Stop a shrunken run of this suite from reading like a complete one.

Most of what makes `flox_mcp` worth testing -- the runtime tools, the DEX
tools, the custom-venue assembly, the 38 tool schemas and the dispatch chain
behind them -- needs either the compiled `flox_py` binding or the `mcp`
package. When one is absent those cases skip, and the summary line of a run
that lost 19 of its cases ("229 passed, 20 skipped") reads exactly like a
healthy one ("248 passed, 1 skipped"). Both exit 0. The difference shows up
only if somebody happens to compare the two numbers, and the cases that drop
out are the ones that touch real code rather than fixtures.

So the dependency state is stated out loud at the end of every run, with the
count, and a run that was supposed to be complete fails outright.

Complete is the default in CI (the `CI` environment variable, which GitHub
Actions sets) and opt-in elsewhere. A contributor who has not built the C++
side still gets a useful run of the pure-Python half; a CI job that thinks it
built the binding and did not gets a red job instead of a green one with a
hole in it. Override either way with `FLOX_MCP_REQUIRE_DEPS=1` / `=0`.
"""
from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_PY = REPO_ROOT / "build" / "python"

# Done here rather than in each test module so every module sees the same
# import state and the probe below reports what the tests will actually get.
if BUILD_PY.is_dir() and str(BUILD_PY) not in sys.path:
    sys.path.insert(0, str(BUILD_PY))

# Dependencies whose absence silently shrinks this suite, and how to get them.
REQUIRED_DEPENDENCIES: dict[str, str] = {
    "flox_py": (
        "the compiled binding -- configure with -DFLOX_BUILD_PYTHON=ON, then "
        "`cmake --build build --target _flox_py`, and run pytest with "
        "PYTHONPATH=build/python"
    ),
    "flox_py.dex": (
        "the DEX comfort layer, which ships inside the binding -- a build that "
        "imports as flox_py but not flox_py.dex is half-installed"
    ),
    "mcp.types": 'the MCP SDK -- `pip install "mcp>=2.0,<3"` (the range in mcp/pyproject.toml)',
}

_MISSING: dict[str, str] = {}
_SKIPS: dict[str, list[str]] = {}
_UNCOLLECTED: dict[str, list[str]] = {}


def _truthy(value: str | None) -> bool:
    return (value or "").strip().lower() in {"1", "true", "yes", "on"}


def _requires_complete_run() -> bool:
    explicit = os.environ.get("FLOX_MCP_REQUIRE_DEPS")
    if explicit is not None and explicit.strip() != "":
        return _truthy(explicit)
    return _truthy(os.environ.get("CI"))


def _probe() -> dict[str, str]:
    missing: dict[str, str] = {}
    for module, how in REQUIRED_DEPENDENCIES.items():
        try:
            importlib.import_module(module)
        except Exception as exc:  # ImportError, but a half-built binding can raise others
            missing[module] = f"{exc.__class__.__name__}: {exc}"
    return missing


def _attribute(reason: str) -> str | None:
    """Which missing dependency a skip reason is talking about, if any.

    Longest name first, so a reason naming `flox_py.dex` is charged to the
    submodule rather than to its parent and counted once.
    """
    lowered = (reason or "").lower()
    for module in sorted(_MISSING, key=len, reverse=True):
        if module in lowered:
            return module
    for module in sorted(_MISSING, key=len, reverse=True):
        # `mcp.types` shows up in skip reasons as plain "mcp".
        if module.split(".")[0] in lowered:
            return module
    return None


def pytest_configure(config: pytest.Config) -> None:
    global _MISSING
    _MISSING = _probe()
    if not _MISSING:
        return

    detail = "\n".join(
        f"  {module} is not importable ({error})\n    {REQUIRED_DEPENDENCIES[module]}"
        for module, error in _MISSING.items()
    )
    if _requires_complete_run():
        raise pytest.UsageError(
            "mcp/tests cannot run completely here, and a partial run of this "
            "suite is indistinguishable from a healthy one:\n"
            f"{detail}\n"
            "Set FLOX_MCP_REQUIRE_DEPS=0 to accept a partial run on purpose."
        )


def pytest_runtest_logreport(report: pytest.TestReport) -> None:
    # A skip raised in the test body reports on "call", one from a marker on
    # "setup". Only one phase of a given test can be skipped, so taking both
    # cannot double-count.
    if report.when not in ("setup", "call") or not report.skipped:
        return
    reason = ""
    if isinstance(report.longrepr, tuple) and len(report.longrepr) == 3:
        reason = str(report.longrepr[2])
    module = _attribute(reason)
    if module:
        _SKIPS.setdefault(module, []).append(report.nodeid)


def pytest_collectreport(report: pytest.CollectReport) -> None:
    """A module skipped at import time takes its whole test count with it.

    pytest counts that as one skip, so the summary understates the loss by
    however many cases the file holds. Count the file's test functions instead.
    """
    if not report.skipped:
        return
    reason = ""
    if isinstance(report.longrepr, tuple) and len(report.longrepr) == 3:
        reason = str(report.longrepr[2])
    module = _attribute(reason)
    if not module:
        return
    path = REPO_ROOT / str(report.nodeid)
    count = 0
    if path.is_file():
        count = sum(1 for line in path.read_text().splitlines() if line.startswith(("def test_", "    def test_")))
    _UNCOLLECTED.setdefault(module, []).extend([str(report.nodeid)] * max(count, 1))


def pytest_terminal_summary(terminalreporter, exitstatus, config) -> None:  # noqa: ANN001
    if not _MISSING:
        return

    write = terminalreporter.write_line
    write("")
    write("=" * 72)
    write("INCOMPLETE RUN -- this suite did not check everything it covers")
    for module, error in _MISSING.items():
        skipped = len(_SKIPS.get(module, []))
        uncollected = len(_UNCOLLECTED.get(module, []))
        total = skipped + uncollected
        write(f"  {module}: {error}")
        write(f"    {total} test case(s) did not run because of it "
              f"({skipped} skipped, {uncollected} never collected)")
        write(f"    {REQUIRED_DEPENDENCIES[module]}")
    write("The pass/skip counts above describe the cases that ran, not the suite.")
    write("=" * 72)

    if _truthy(os.environ.get("GITHUB_ACTIONS")):
        for module in _MISSING:
            total = len(_SKIPS.get(module, [])) + len(_UNCOLLECTED.get(module, []))
            print(f"::warning::mcp/tests ran without {module}; {total} test case(s) did not run")
