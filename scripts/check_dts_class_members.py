#!/usr/bin/env python3
"""Check that node/index.d.ts class bodies cover every NAPI wrap method.

T018 / T021 / T022 / T026 each introduced new NAPI InstanceMethod
entries and silently left the d.ts class body behind. The
check_dts_exports.py gate catches missing top-level exports but not
missing methods inside an existing class body. This script closes that
gap.

For each `*Wrap` class in `node/src/*.h`:
  - Parse the DefineClass(...) block to extract InstanceMethod("name", ...).
  - Resolve the wrap class to its TS export name (strip the trailing Wrap).
  - Find the matching `export class <Name>` in node/index.d.ts.
  - Verify every InstanceMethod name appears as a method in that class body.

Conservative reverse direction: extra d.ts methods are fine (some are
intentionally hand-written aliases / overloads). Missing methods fail
the check.

The class-name alias map handles cases where the d.ts name differs from
the wrap class name (rare).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
NODE_SRC = REPO_ROOT / "node" / "src"
DTS_PATH = REPO_ROOT / "node" / "index.d.ts"

# (wrap class name with trailing "Wrap" stripped) -> d.ts export name.
# Only entries for names that don't follow the strip-Wrap rule.
DTS_ALIASES: dict[str, str] = {}

# Wrap classes whose methods should not be required in d.ts (test
# fixtures, internal helpers). Empty by default.
SKIP_WRAPS: set[str] = set()

INSTANCE_METHOD_RE = re.compile(r'InstanceMethod\(\s*"([A-Za-z0-9_]+)"')
CLASS_WRAP_RE = re.compile(r"class\s+([A-Za-z0-9_]+Wrap)\s*:")
EXPORT_CLASS_RE = re.compile(r"^export\s+class\s+([A-Za-z0-9_]+)\s*\{")
TS_METHOD_RE = re.compile(r"^\s*(?:readonly\s+)?([A-Za-z0-9_]+)\s*[(:]")


def parse_wrap_methods(text: str) -> dict[str, set[str]]:
    """{wrap_class -> set(InstanceMethod names)} for one source file."""
    out: dict[str, set[str]] = {}
    cur_class: str | None = None
    brace_depth = 0
    for line in text.splitlines():
        m = CLASS_WRAP_RE.search(line)
        if m:
            cur_class = m.group(1)
            out.setdefault(cur_class, set())
            brace_depth = line.count("{") - line.count("}")
            continue
        if cur_class is None:
            continue
        brace_depth += line.count("{") - line.count("}")
        for nm in INSTANCE_METHOD_RE.findall(line):
            out[cur_class].add(nm)
        if brace_depth <= 0 and "}" in line:
            cur_class = None
            brace_depth = 0
    return out


def parse_dts_classes(text: str) -> dict[str, set[str]]:
    """{class_name -> set(method names)} for index.d.ts."""
    out: dict[str, set[str]] = {}
    cur: str | None = None
    depth = 0
    for line in text.splitlines():
        if cur is None:
            m = EXPORT_CLASS_RE.match(line)
            if m:
                cur = m.group(1)
                out.setdefault(cur, set())
                depth = line.count("{") - line.count("}")
            continue
        depth += line.count("{") - line.count("}")
        m = TS_METHOD_RE.match(line)
        if m:
            name = m.group(1)
            if name in {"constructor"} or name.startswith("//"):
                pass
            elif not any(kw in line for kw in ("readonly ", "// ")):
                out[cur].add(name)
            elif "readonly " in line:
                out[cur].add(name)
            else:
                out[cur].add(name)
        if depth <= 0:
            cur = None
    return out


def wrap_to_dts_name(wrap: str) -> str:
    base = wrap[:-4] if wrap.endswith("Wrap") else wrap
    return DTS_ALIASES.get(base, base)


def main() -> int:
    if not DTS_PATH.exists():
        print(f"::error::{DTS_PATH} not found", file=sys.stderr)
        return 2
    dts_classes = parse_dts_classes(DTS_PATH.read_text(encoding="utf-8"))

    failures: list[str] = []
    coverage: list[str] = []

    for src in sorted(NODE_SRC.glob("*.h")):
        wraps = parse_wrap_methods(src.read_text(encoding="utf-8"))
        for wrap, methods in wraps.items():
            if wrap in SKIP_WRAPS or not methods:
                continue
            dts_name = wrap_to_dts_name(wrap)
            dts_methods = dts_classes.get(dts_name)
            if dts_methods is None:
                coverage.append(
                    f"  {wrap:<40} -> {dts_name}: NO d.ts class (skipped)"
                )
                continue
            missing = methods - dts_methods
            coverage.append(
                f"  {wrap:<40} -> {dts_name}: {len(methods)} napi / "
                f"{len(missing)} missing"
            )
            if missing:
                failures.append(
                    f"::error::{src.name} class {wrap} -> d.ts class {dts_name} "
                    f"is missing methods: {', '.join(sorted(missing))}"
                )

    print("NAPI -> d.ts class member coverage:")
    for line in coverage:
        print(line)
    print()

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        return 1
    print(f"OK — every NAPI InstanceMethod is declared in node/index.d.ts.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
