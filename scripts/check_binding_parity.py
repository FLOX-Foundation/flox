#!/usr/bin/env python3
"""Cross-binding parity gate.

Reads the IDL spec (`include/flox/capi/flox_capi_spec.hpp`) and verifies
that every user-facing C ABI group has a corresponding presence in each
language binding (pybind11 Python, NAPI Node, Codon, optionally QuickJS).

Configuration lives in `tools/codegen/binding_parity.yaml`. Each group
declared in IDL must be listed there, with a per-binding status:

    required: a mapping with `classes` / `functions` that must exist
    not_applicable: this group is internal / never exposed to this binding
    allowlist: known gap, with `reason` (links to a tracker task)

The script fails CI when:
    - A group exists in IDL but isn't in the YAML
    - A `required` binding is missing one of the declared classes/functions
    - An `allowlist` entry is missing a reason

Run as:
    python3 scripts/check_binding_parity.py

Or with --verbose for details on every group.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parent.parent
IDL_PATH = ROOT / "include" / "flox" / "capi" / "flox_capi_spec.hpp"
CONFIG_PATH = ROOT / "tools" / "codegen" / "binding_parity.yaml"
PYI_PATH = ROOT / "python" / "flox_py" / "_flox_py" / "__init__.pyi"
DTS_PATH = ROOT / "node" / "index.d.ts"
CODON_GOLDEN_PATH = ROOT / "tools" / "codegen" / "golden" / "flox_capi.codon"


# ── IDL parsing ────────────────────────────────────────────────────────


@dataclass
class IdlGroup:
    name: str
    functions: list[str]


def parse_idl_groups(text: str) -> list[IdlGroup]:
    """Walk the IDL spec, gathering function declarations per group.

    Each FLOX_EXPORT(group = "X") attaches to the next function. Functions
    span multiple lines (parameters often broken). We track the current
    group via the most recently seen FLOX_EXPORT, then capture the
    function name from the line that follows."""
    groups: dict[str, list[str]] = {}
    current_group: str | None = None
    fn_pattern = re.compile(r"\b(flox_[a-z_0-9]+)\s*\(")
    group_pattern = re.compile(r'group\s*=\s*"([^"]+)"')
    for line in text.splitlines():
        s = line.strip()
        if "FLOX_EXPORT" in s:
            m = group_pattern.search(s)
            if m:
                current_group = m.group(1)
            continue
        if current_group is not None:
            m = fn_pattern.search(s)
            if m:
                groups.setdefault(current_group, []).append(m.group(1))
                current_group = None  # consumed
    return [IdlGroup(name=k, functions=sorted(set(v))) for k, v in sorted(groups.items())]


# ── Binding scanners ───────────────────────────────────────────────────


def scan_pyi(path: Path) -> tuple[set[str], set[str]]:
    """Return (classes, top-level functions) declared in the .pyi file."""
    if not path.exists():
        return set(), set()
    classes: set[str] = set()
    funcs: set[str] = set()
    for line in path.read_text().splitlines():
        m = re.match(r"^class\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            classes.add(m.group(1))
            continue
        m = re.match(r"^def\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            funcs.add(m.group(1))
    return classes, funcs


def scan_dts(path: Path) -> tuple[set[str], set[str]]:
    """Return (classes-and-interfaces, top-level functions) declared in
    the .d.ts file. NAPI hooks are typically modelled as `interface` (a
    plain object with named method properties) rather than `class`, so
    we accept both for the parity gate."""
    if not path.exists():
        return set(), set()
    classes, funcs = set(), set()
    for line in path.read_text().splitlines():
        m = re.match(r"^export\s+(class|interface)\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            classes.add(m.group(2))
            continue
        m = re.match(r"^export\s+function\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            funcs.add(m.group(1))
    return classes, funcs


def scan_codon_groups(path: Path) -> set[str]:
    """Codon golden file is grouped by '# ── group_name ──' comments. We
    take any group present in the file as covered (the file is generated
    from IDL, so anything emitted in IDL ends up here)."""
    if not path.exists():
        return set()
    seen: set[str] = set()
    pat = re.compile(r"^#\s*──\s*([A-Za-z_0-9]+)\s*──\s*$")
    for line in path.read_text().splitlines():
        m = pat.match(line)
        if m:
            seen.add(m.group(1))
    return seen


# ── Verification ───────────────────────────────────────────────────────


@dataclass
class GroupReport:
    group: str
    binding: str
    status: str  # ok | missing_yaml | missing_in_binding | allowlist_no_reason
    detail: str = ""


def verify_classes_and_funcs(group: str, binding: str, expect: dict,
                              present_classes: set[str], present_funcs: set[str]) -> list[GroupReport]:
    status = expect.get("status")
    if status == "not_applicable":
        return [GroupReport(group, binding, "ok", "n/a")]
    if status == "allowlist":
        reason = (expect.get("reason") or "").strip()
        if not reason:
            return [GroupReport(group, binding, "allowlist_no_reason",
                                "allowlist entry must include a `reason`")]
        return [GroupReport(group, binding, "ok", f"allowlist: {reason}")]
    if status != "required":
        return [GroupReport(group, binding, "missing_yaml",
                            f"unknown status `{status}`; expected required/not_applicable/allowlist")]
    expected_classes = expect.get("classes") or []
    expected_funcs = expect.get("functions") or []
    if not expected_classes and not expected_funcs:
        return [GroupReport(group, binding, "missing_yaml",
                            "required status but no classes/functions listed")]
    missing_c = [c for c in expected_classes if c not in present_classes]
    missing_f = [f for f in expected_funcs if f not in present_funcs]
    if missing_c or missing_f:
        bits = []
        if missing_c:
            bits.append(f"classes={missing_c}")
        if missing_f:
            bits.append(f"functions={missing_f}")
        return [GroupReport(group, binding, "missing_in_binding",
                            "missing " + ", ".join(bits))]
    return [GroupReport(group, binding, "ok",
                        f"classes={expected_classes} funcs={expected_funcs}")]


def verify_codon(group: str, expect: dict, codon_groups: set[str]) -> list[GroupReport]:
    status = expect.get("status")
    if status == "not_applicable":
        return [GroupReport(group, "codon", "ok", "n/a")]
    # Codon is auto-generated from IDL: presence of a group section in the
    # golden file is a sufficient check (function-level granularity is
    # already covered by codegen-check).
    if group in codon_groups:
        return [GroupReport(group, "codon", "ok", "section present in golden")]
    if status == "allowlist":
        return [GroupReport(group, "codon", "ok",
                            f"allowlist: {expect.get('reason', '')}")]
    return [GroupReport(group, "codon", "missing_in_binding",
                        f"group not found in {CODON_GOLDEN_PATH.name}")]


# ── Main ──────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--verbose", action="store_true",
                    help="print every group, not only failures")
    args = ap.parse_args(argv)

    if not IDL_PATH.exists():
        print(f"ERROR: IDL spec not found: {IDL_PATH}", file=sys.stderr)
        return 2
    if not CONFIG_PATH.exists():
        print(f"ERROR: parity config not found: {CONFIG_PATH}", file=sys.stderr)
        return 2

    idl_groups = parse_idl_groups(IDL_PATH.read_text())
    idl_names = {g.name for g in idl_groups}

    config = yaml.safe_load(CONFIG_PATH.read_text()) or {}
    cfg_groups = config.get("groups", {})

    pyi_classes, pyi_funcs = scan_pyi(PYI_PATH)
    dts_classes, dts_funcs = scan_dts(DTS_PATH)
    codon_groups = scan_codon_groups(CODON_GOLDEN_PATH)

    reports: list[GroupReport] = []

    # 1. Every IDL group must appear in the config.
    unknown_in_yaml = sorted(idl_names - set(cfg_groups.keys()))
    for g in unknown_in_yaml:
        reports.append(GroupReport(g, "config", "missing_yaml",
                                   f"add an entry to {CONFIG_PATH.name}"))

    # 2. Config entries for groups that don't exist in IDL.
    stale_in_yaml = sorted(set(cfg_groups.keys()) - idl_names)
    for g in stale_in_yaml:
        reports.append(GroupReport(g, "config", "missing_in_binding",
                                   f"group `{g}` not found in IDL spec; remove from yaml"))

    # 3. Per-group, per-binding verification.
    for group_name in sorted(idl_names & set(cfg_groups.keys())):
        entry = cfg_groups[group_name] or {}
        reports += verify_classes_and_funcs(group_name, "pybind11",
                                              entry.get("pybind11", {"status": "missing_yaml"}),
                                              pyi_classes, pyi_funcs)
        reports += verify_classes_and_funcs(group_name, "napi",
                                              entry.get("napi", {"status": "missing_yaml"}),
                                              dts_classes, dts_funcs)
        reports += verify_codon(group_name,
                                 entry.get("codon", {"status": "missing_yaml"}),
                                 codon_groups)

    # ── Output ─────────────────────────────────────────────────────────

    failures = [r for r in reports if r.status != "ok"]
    if args.verbose or failures:
        print("Cross-binding parity report")
        print("─" * 60)
        if args.verbose:
            for r in reports:
                tag = "OK   " if r.status == "ok" else "FAIL "
                print(f"{tag} {r.group:30s} {r.binding:10s} {r.detail}")
        else:
            for r in failures:
                print(f"FAIL  {r.group:30s} {r.binding:10s} {r.status}: {r.detail}")

    if failures:
        print()
        print(f"{len(failures)} parity issue(s) found.")
        print(f"Edit {CONFIG_PATH.relative_to(ROOT)} to declare expected coverage,")
        print("or add bindings for the missing groups.")
        return 1

    print(f"OK — {len(idl_names)} groups, all bindings in parity.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
