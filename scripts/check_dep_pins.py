#!/usr/bin/env python3
"""Fail when CI installs a different dependency range than the package declares.

A tighter bound in CI than in the manifest means CI is green against versions
users never get, and a new major release of the dependency breaks every fresh
install with nothing in the repo able to notice -- the tested range and the
shipped range are just different strings in different files.

This gate reads the ranges out of the packaging manifests and out of the CI
workflow and requires them to agree, so a bound can only be relaxed in both
places at once.
"""
from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CI = ROOT / ".github" / "workflows" / "ci.yml"

# Packages worth guarding: third-party runtime deps whose major version can
# break us. Extend as new ones appear.
GUARDED = {"mcp"}


def _norm(spec: str) -> str:
    """Compare specifiers without whitespace or quoting noise."""
    return re.sub(r"[\s\"']", "", spec)


def _declared() -> dict[str, tuple[str, Path]]:
    out: dict[str, tuple[str, Path]] = {}
    for manifest in ROOT.rglob("pyproject.toml"):
        if any(part in {"external", "build", "node_modules", ".venv"} for part in manifest.parts):
            continue
        data = tomllib.loads(manifest.read_text())
        project = data.get("project", {})
        specs = list(project.get("dependencies", []) or [])
        # Extras count too: flox-py declares `mcp` under optional-dependencies,
        # and an unbounded range there breaks `pip install flox-py[mcp]` just
        # as thoroughly as one in the main list.
        for extra in (project.get("optional-dependencies", {}) or {}).values():
            specs.extend(extra or [])
        for dep in specs:
            name = re.split(r"[<>=!~\[ ]", dep, maxsplit=1)[0].strip().lower()
            if name in GUARDED:
                key = f"{name} ({manifest.parent.name})" if name in out else name
                out[key] = (_norm(dep), manifest.relative_to(ROOT))
    return out


def _installed_by_ci() -> dict[str, tuple[str, int]]:
    out: dict[str, tuple[str, int]] = {}
    if not CI.exists():
        return out
    for lineno, line in enumerate(CI.read_text().splitlines(), 1):
        if "pip install" not in line:
            continue
        for spec in re.findall(r"[\"']([A-Za-z0-9_.-]+(?:[<>=!~][^\"']*)?)[\"']", line):
            name = re.split(r"[<>=!~\[ ]", spec, maxsplit=1)[0].strip().lower()
            if name in GUARDED:
                out[name] = (_norm(spec), lineno)
    return out


def main() -> int:
    declared = _declared()
    ci = _installed_by_ci()
    problems: list[str] = []

    for key in sorted(declared):
        name = key.split(" ")[0]
        spec, manifest = declared[key]
        if name not in ci:
            problems.append(
                f"{name}: declared as {spec} in {manifest}, but CI never installs it explicitly -- "
                f"the tested version is whatever a transitive resolve picks")
            continue
        ci_spec, lineno = ci[name]
        if spec != ci_spec:
            problems.append(
                f"{name}: {manifest} declares {spec}, CI installs {ci_spec} "
                f"(.github/workflows/ci.yml:{lineno}). CI is testing a range users do not get.")

    if problems:
        print("dependency ranges disagree between the package and CI:")
        for p in problems:
            print(f"  {p}")
        print("\nMake both sides identical, then re-run.")
        return 1

    print(f"OK: {len(declared)} guarded dependency range(s) match between manifests and CI.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
