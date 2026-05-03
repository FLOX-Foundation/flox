#!/usr/bin/env python3
"""
scripts/check_dts_exports.py

Source-level sync gate for the Node.js binding's TypeScript declarations.

Walks `node/src/*.h` and collects every name registered on the module
exports object (`exports.Set("X", ...)`) and on the `targets` submodule
(`targets.Set("X", ...)`). Walks `node/index.d.ts` and collects every
top-level declared name plus every name declared inside
`export namespace targets { ... }`. Fails if the two sets differ.

Why this exists: pybind11-stubgen for Python catches drift automatically;
node-addon-api has no comparable tool. Without this gate a new C++ binding
is silently undocumented for TypeScript users.

This catches name-level drift (added / removed exports). It does NOT
catch signature drift; the companion `node/test/test_types.ts` exercises
every export and `tsc --noEmit` flags signature mismatches there.

Usage:
    python3 scripts/check_dts_exports.py        # report and exit 1 on drift
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC_DIR = REPO / "node" / "src"
DTS = REPO / "node" / "index.d.ts"

# C++ side: `<obj>.Set("name", ...)` where obj is `exports` or `targets`.
RE_CPP_SET = re.compile(r"\b(exports|targets)\.Set\(\s*\"([^\"]+)\"")

# TS side: top-level `export class|function|const|namespace X` and
# `export interface|type X` (interfaces and types are explicitly *not*
# binding exports, so they're excluded from the comparison set).
RE_TS_EXPORT = re.compile(
    r"^export\s+(?:declare\s+)?(class|function|const|namespace)\s+(\w+)",
    re.M,
)
# Names registered inside the `targets` namespace block.
RE_TS_TARGETS_NAMESPACE = re.compile(
    r"export\s+namespace\s+targets\s*\{([^}]*)\}",
    re.S,
)
RE_TS_NAMESPACE_MEMBER = re.compile(
    r"^\s*(?:function|class|const)\s+(\w+)",
    re.M,
)


def collect_cpp() -> tuple[set[str], set[str]]:
    top: set[str] = set()
    targets: set[str] = set()
    for header in sorted(SRC_DIR.glob("*.h")):
        for obj, name in RE_CPP_SET.findall(header.read_text()):
            (targets if obj == "targets" else top).add(name)
    return top, targets


def collect_ts() -> tuple[set[str], set[str]]:
    text = DTS.read_text()
    top = {name for kind, name in RE_TS_EXPORT.findall(text) if kind != "namespace"}
    # Add the `targets` namespace itself if declared (it's an export).
    if any(kind == "namespace" and name == "targets" for kind, name in RE_TS_EXPORT.findall(text)):
        top.add("targets")
    targets: set[str] = set()
    m = RE_TS_TARGETS_NAMESPACE.search(text)
    if m:
        targets = set(RE_TS_NAMESPACE_MEMBER.findall(m.group(1)))
    return top, targets


def report_diff(label: str, cpp: set[str], ts: set[str]) -> bool:
    missing_in_ts = cpp - ts
    missing_in_cpp = ts - cpp
    if not missing_in_ts and not missing_in_cpp:
        return True
    if missing_in_ts:
        print(
            f"::error::{label}: declared in node/src but missing from "
            f"node/index.d.ts: {sorted(missing_in_ts)}",
            file=sys.stderr,
        )
    if missing_in_cpp:
        print(
            f"::error::{label}: declared in node/index.d.ts but missing "
            f"from node/src: {sorted(missing_in_cpp)}",
            file=sys.stderr,
        )
    return False


def main() -> int:
    cpp_top, cpp_targets = collect_cpp()
    ts_top, ts_targets = collect_ts()
    ok_top = report_diff("top-level exports", cpp_top, ts_top)
    ok_targets = report_diff("targets.* exports", cpp_targets, ts_targets)
    if not (ok_top and ok_targets):
        print(
            "Edit node/index.d.ts to add/remove the listed names, then re-run.",
            file=sys.stderr,
        )
        return 1
    print(
        f"node/index.d.ts in sync: {len(cpp_top)} top-level + "
        f"{len(cpp_targets)} targets.* exports."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
