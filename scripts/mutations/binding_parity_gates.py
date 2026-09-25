#!/usr/bin/env python3
"""Mutation harness for the binding parity/smoke gates and the generated
event-struct layouts.

Covers, one piece at a time:

    scripts/check_binding_parity.py       function-level parity per binding,
                                          --require-quickjs, --root, the
                                          allowlist, the QuickJS name map
    scripts/check_binding_smoke.py        the fatal-import path, the missing
                                          node/index.js path, the exit code
    tools/codegen/flox_codegen/emit_layout.py
                                          offset/alignment/nesting/array/
                                          padding computation for the two
                                          generated layout artifacts
    codon/flox/layout.codon               a generated-but-committed artifact,
    codon/flox/dispatch.codon             direct drift simulation
    include/flox/capi/flox_capi_layout.h  ditto, checked against offsetof
    .github/workflows/ci.yml              the two gate-wiring assertions

Three kinds of check answer for a mutation, depending on where it lands:

    pytest    run the pytest node id(s) named for the mutation; on green,
              fall back to the full sweep below before calling it alive
    codegen   mutate tools/codegen/flox_codegen/emit_layout.py, then run
              tools/codegen/scripts/check.sh -- it regenerates both layout
              artifacts into temp files and diffs them against the committed
              ones (plus runs tools/codegen/tests itself), so a real
              behavioural change shows up as drift without ever touching a
              tracked file
    cpp       mutate include/flox/capi/flox_capi_layout.h directly (it is
              generated, but the mutation simulates the drift check.sh exists
              to catch), delete test_capi_event_layout's object files, rebuild,
              and run the gtest binary

Every source-file mutation is restored by writing the original text back and
re-hashing to confirm it matches. Direct edits to a *generated* file
(flox_capi_layout.h, codon/flox/layout.codon) are restored with
`git show HEAD:path > path` instead, never `git checkout --`, per policy.

Unexplained survivors (green everywhere, no `equivalent_reason`) fail the run.

Usage:

    python3 scripts/mutations/binding_parity_gates.py
    python3 scripts/mutations/binding_parity_gates.py --list
    python3 scripts/mutations/binding_parity_gates.py --only mut-codon-reads-golden
    python3 scripts/mutations/binding_parity_gates.py --skip-sweep

Expected environment (already set up by this run):
    tools/codegen/.venv                          (tools/codegen/setup.sh)
    build/ configured with
        cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
              -DFLOX_BUILD_TESTS=ON -DFLOX_BUILD_CAPI=ON -DFLOX_NATIVE=OFF
    build/tests/test_capi_event_layout already built once
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 600
TEST_TIMEOUT = 120
PYTEST_TIMEOUT = 120
CHECK_SH_TIMEOUT = 180

PYTHON = sys.executable
CPP_TARGET = "test_capi_event_layout"

PARITY = "scripts/check_binding_parity.py"
SMOKE = "scripts/check_binding_smoke.py"
EMIT_LAYOUT = "tools/codegen/flox_codegen/emit_layout.py"
LAYOUT_CODON = "codon/flox/layout.codon"
DISPATCH_CODON = "codon/flox/dispatch.codon"
LAYOUT_HEADER = "include/flox/capi/flox_capi_layout.h"
CI_YAML = ".github/workflows/ci.yml"

FULL_SCRIPTS_TESTS = "scripts/tests"
FULL_CODEGEN_TESTS = "tools/codegen/tests"


@dataclass
class Mutation:
    name: str
    why: str
    file: str
    old: str
    new: str
    kind: str = "pytest"          # pytest | codegen | cpp
    primary_tests: tuple = ()     # pytest node ids, relative to REPO
    restore_kind: str = "write"   # write | git-show
    equivalent_reason: str = ""
    expected_occurrences: int = 1


MUTATIONS: list[Mutation] = [

    # ── scripts/check_binding_parity.py ─────────────────────────────────
    Mutation(
        name="mut-parity-class-names-only-pybind11",
        why="verify_binding stops calling verify_functions for pybind11, so the group is "
            "satisfied again by the class name in the .pyi alone -- the exact regression the "
            "gate exists to catch, reintroduced for one binding",
        file=PARITY,
        old='''    reports += verify_functions(group, binding, expect, idl_functions,
                                surface.reachable[binding], allowlist)''',
        new='''    if binding != "pybind11":
        reports += verify_functions(group, binding, expect, idl_functions,
                                    surface.reachable[binding], allowlist)''',
        primary_tests=(
            f"{PARITY.replace('scripts/', 'scripts/tests/test_binding_')}",  # placeholder, overwritten below
        ),
    ),
    Mutation(
        name="mut-parity-comments-not-stripped",
        why="scan_c_call_sites stops stripping block/line comments before scanning, so a "
            "function named only in a comment counts as a reachable call site",
        file=PARITY,
        old='''        text = _BLOCK_COMMENT.sub(" ", text)
        text = _LINE_COMMENT.sub(" ", text)
        found |= set(_C_FUNCTION.findall(text))''',
        new='''        found |= set(_C_FUNCTION.findall(text))''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_a_function_dropped_from_the_pybind11_source_turns_the_gate_red",),
        equivalent_reason="",  # filled in after the run if it survives
    ),
    Mutation(
        name="mut-codon-reads-golden",
        why="scan_codon_imports ignores the `tree` it was called with (codon/flox/, the "
            "shipped module) and reads tools/codegen/golden/flox_capi.codon instead -- the "
            "file generated from the same IDL the gate just parsed, which cannot disagree "
            "with it, exactly the bug the parity gate was rewritten to stop trusting",
        file=PARITY,
        old='''def scan_codon_imports(tree: Path) -> set[str]:
    """C API functions imported by the shipped Codon modules.

    `from C import flox_x(...)` is the only way Codon can call into the C
    ABI, so it is what "Codon reaches this function" means. Read from
    `codon/flox/`, deliberately not from the IDL-generated golden: the
    golden is generated from the same spec this gate parses, so a check
    against it can never fail.
    """
    found: set[str] = set()
    if not tree.is_dir():
        return found
    for path in sorted(tree.rglob("*.codon")):
        _groups, imports = codegen_manifest.scan_codon(
            path.read_text(encoding="utf-8", errors="ignore"))
        found |= set(imports)
    return found''',
        new='''def scan_codon_imports(tree: Path) -> set[str]:
    found: set[str] = set()
    golden = tree.parent.parent / "tools" / "codegen" / "golden" / "flox_capi.codon"
    if not golden.exists():
        return found
    _groups, imports = codegen_manifest.scan_codon(
        golden.read_text(encoding="utf-8", errors="ignore"))
    found |= set(imports)
    return found''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_the_codon_half_reads_the_shipped_module_and_not_the_golden",),
    ),
    Mutation(
        name="mut-quickjs-map-checked-against-nothing",
        why="the quickjs branch of verify_functions stops checking whether the mapped global "
            "is actually registered with addGlobalFunc -- every mapping entry is accepted "
            "purely for existing in the yaml, whatever it points at",
        file=PARITY,
        old='''        if binding == "quickjs":
            if fn in mapping:
                global_name = str(mapping[fn])
                if global_name not in reachable:
                    unregistered.append(f"{fn} -> {global_name}")
                continue
        elif fn in reachable:
            continue''',
        new='''        if binding == "quickjs":
            if fn in mapping:
                global_name = str(mapping[fn])
                continue
        elif fn in reachable:
            continue''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_a_new_c_function_with_no_wrapper_is_red_in_all_four_bindings",),
    ),
    Mutation(
        name="mut-quickjs-unmapped-function-silently-passes",
        why="a C function with no entry at all in the quickjs `functions:` map now short-"
            "circuits with `continue` instead of falling through to the allowlist check -- an "
            "unmapped function is treated as fine rather than missing",
        file=PARITY,
        old='''        if binding == "quickjs":
            if fn in mapping:
                global_name = str(mapping[fn])
                if global_name not in reachable:
                    unregistered.append(f"{fn} -> {global_name}")
                continue
        elif fn in reachable:
            continue''',
        new='''        if binding == "quickjs":
            if fn not in mapping:
                continue
            global_name = str(mapping[fn])
            if global_name not in reachable:
                unregistered.append(f"{fn} -> {global_name}")
            continue
        elif fn in reachable:
            continue''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_a_new_c_function_with_no_wrapper_is_red_in_all_four_bindings",),
    ),
    Mutation(
        name="mut-require-quickjs-default-flipped",
        why="--require-quickjs's default flips from True to False, so a plain CI invocation "
            "with no flag once again leaves every group unchecked against QuickJS",
        file=PARITY,
        old='''    default=True,
                    help="fail on a group with no `quickjs` entry (default: on)")''',
        new='''    default=False,
                    help="fail on a group with no `quickjs` entry (default: on)")''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_require_quickjs_is_on_by_default",),
    ),
    Mutation(
        name="mut-root-ignored-for-pyi",
        why="Paths.pyi stops resolving under self.root and reads the checkout's own .pyi "
            "instead -- --root is honoured for every other path property but silently "
            "ignored for this one data path",
        file=PARITY,
        old='''    @property
    def pyi(self) -> Path:
        return self.root / "python" / "flox_py" / "_flox_py" / "__init__.pyi"''',
        new='''    @property
    def pyi(self) -> Path:
        return SCRIPT_ROOT / "python" / "flox_py" / "_flox_py" / "__init__.pyi"''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_a_function_dropped_from_the_pybind11_source_turns_the_gate_red",),
    ),
    Mutation(
        name="mut-allowlisted-function-also-required",
        why="an allowlist entry with a valid reason is now *also* appended to `missing` -- "
            "double counting -- so every one of the ~1700 allowlisted functions across all "
            "four bindings fails the gate it was declared to satisfy",
        file=PARITY,
        old='''        listed, reason = _allowlisted(allowlist, fn)
        if not listed:
            missing.append(fn)
        elif not reason:
            no_reason.append(fn)''',
        new='''        listed, reason = _allowlisted(allowlist, fn)
        if not listed:
            missing.append(fn)
        elif not reason:
            no_reason.append(fn)
        else:
            missing.append(fn)  # BUG: allowlisted-with-reason is double counted as missing''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_the_real_repository_passes_its_own_gate",),
    ),
    Mutation(
        name="mut-quickjs-empty-function-map-passes",
        why="a `required` quickjs entry whose `functions:` map is empty (or absent) now "
            "returns immediately with no reports -- a group nobody ever mapped is treated as "
            "fully satisfied instead of every one of its functions being unmapped",
        file=PARITY,
        old='''    mapping = expect.get("functions") if binding == "quickjs" else None
    if not isinstance(mapping, dict):
        mapping = {}

    for fn in idl_functions:''',
        new='''    mapping = expect.get("functions") if binding == "quickjs" else None
    if not isinstance(mapping, dict):
        mapping = {}
    if binding == "quickjs" and not mapping:
        return []

    for fn in idl_functions:''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_require_quickjs_is_on_by_default",),
    ),
    Mutation(
        name="mut-config-missing-group-silently-ok",
        why="the check that every IDL group must appear in tools/codegen/binding_parity.yaml "
            "is removed -- a group that grows in the IDL and is never declared passes with no "
            "warning at all",
        file=PARITY,
        old='''    # 1. Every IDL group must appear in the config.
    for g in sorted(idl_names - set(cfg_groups.keys())):
        reports.append(GroupReport(g, "config", "missing_yaml",
                                   f"add an entry to {paths.config.name}"))''',
        new='''    # 1. Every IDL group must appear in the config.
    pass''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_a_new_c_function_with_no_wrapper_is_red_in_all_four_bindings",),
    ),
    Mutation(
        name="mut-quickjs-unregistered-check-prefix-match",
        why="the quickjs global-registration check switches from exact membership to a "
            "startswith scan -- a mapped global satisfied by any registered name it is a "
            "prefix of, not only its own registration",
        file=PARITY,
        old='''                if global_name not in reachable:
                    unregistered.append(f"{fn} -> {global_name}")''',
        new='''                if not any(r.startswith(global_name) for r in reachable):
                    unregistered.append(f"{fn} -> {global_name}")''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_a_new_c_function_with_no_wrapper_is_red_in_all_four_bindings",),
    ),
    Mutation(
        name="mut-call-site-prefix-match",
        why="the pybind11/napi/codon reachability check switches from exact membership in the "
            "scanned call-site set to a startswith scan -- a function is satisfied by any call "
            "site it is a text prefix of, not only a call to itself",
        file=PARITY,
        old='''        elif fn in reachable:
            continue''',
        new='''        elif any(r.startswith(fn) for r in reachable):
            continue''',
        primary_tests=("scripts/tests/test_binding_parity_gate.py::"
                       "test_a_function_dropped_from_the_pybind11_source_turns_the_gate_red",),
    ),

    # ── scripts/check_binding_smoke.py ──────────────────────────────────
    Mutation(
        name="mut-smoke-fatal-back-to-skip",
        why="a Python binding that raises on import goes back to a SKIP print and an empty "
            "problem list -- the exact regression this gate was rewritten to stop making",
        file=SMOKE,
        old='''    if "fatal" in data:
        # A binding that will not import is the largest binding-level failure
        # there is: nothing below was exercised. This used to print SKIP and
        # return an empty list, so a broken wheel read as "no failures".
        print(f"  python: FAIL ({data['fatal']})")
        return [f"python: {data['fatal']}"]''',
        new='''    if "fatal" in data:
        print(f"  python: SKIP ({data['fatal']})")
        return []''',
        primary_tests=("scripts/tests/test_binding_smoke_gate.py::"
                       "test_a_python_binding_that_cannot_be_imported_is_a_failure",),
    ),
    Mutation(
        name="mut-smoke-node-missing-back-to-skip",
        why="a missing node/index.js (the addon was never built) goes back to SKIP and an "
            "empty problem list instead of naming the missing entry point as a failure",
        file=SMOKE,
        old='''    if not entry.is_file():
        # Same defect as an addon that throws on load: nothing was exercised.
        print(f"  node: FAIL ({entry} not found — the addon was never built)")
        return [f"node: {entry} not found — the addon was never built"]''',
        new='''    if not entry.is_file():
        print(f"  node: SKIP ({entry} not found — the addon was never built)")
        return []''',
        primary_tests=("scripts/tests/test_binding_smoke_gate.py::"
                       "test_a_binding_that_was_never_built_is_a_failure",),
    ),
    Mutation(
        name="mut-smoke-node-throw-back-to-skip",
        why="a node addon whose entry point throws on require() goes back to SKIP -- the "
            "require-time crash this gate exists to catch is swallowed again",
        file=SMOKE,
        old='''    if "fatal" in data:
        # The message carries a require stack; print the headline here and
        # keep the whole thing for the failure list below.
        print(f"  node: FAIL (cannot load {entry})")
        return [f"node: cannot load {entry}: {data['fatal']}"]''',
        new='''    if "fatal" in data:
        print(f"  node: SKIP (cannot load {entry})")
        return []''',
        primary_tests=("scripts/tests/test_binding_smoke_gate.py::"
                       "test_a_node_addon_that_cannot_be_loaded_is_a_failure",),
    ),
    Mutation(
        name="mut-smoke-exit-0-on-problems",
        why="main() returns 0 even when binding-level problems were collected -- the gate "
            "prints the failures and exits clean anyway",
        file=SMOKE,
        old='''    if problems:
        print("\\nerror: binding-level failures — the binding is broken, not "
              "the input:\\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1''',
        new='''    if problems:
        print("\\nerror: binding-level failures — the binding is broken, not "
              "the input:\\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 0''',
        primary_tests=("scripts/tests/test_binding_smoke_gate.py::"
                       "test_a_python_binding_that_cannot_be_imported_is_a_failure",),
    ),

    # ── tools/codegen/flox_codegen/emit_layout.py (checked via check.sh) ─
    Mutation(
        name="mut-emit-layout-offset-off-by-one",
        why="every field offset gets +1 past the alignment rounding -- every field in every "
            "struct moves",
        file=EMIT_LAYOUT,
        old='''        offset = (offset + align - 1) // align * align''',
        new='''        offset = (offset + align - 1) // align * align + 1''',
        kind="codegen",
    ),
    Mutation(
        name="mut-emit-layout-alignment-rounding-dropped",
        why="a struct's size is no longer rounded up to its widest member's alignment -- it "
            "is left at the raw offset after the last field",
        file=EMIT_LAYOUT,
        old='''    size = (offset + alignment - 1) // alignment * alignment''',
        new='''    size = offset''',
        kind="codegen",
    ),
    Mutation(
        name="mut-emit-layout-nested-struct-flattened",
        why="a nested struct field (FloxSymbolContext.book) is laid out using its own size but "
            "alignment 1 instead of its real alignment -- the nested struct is effectively "
            "flattened byte-wise instead of respecting its own layout",
        file=EMIT_LAYOUT,
        old='''    if spelling in structs:
        nested = _layout_struct(spelling, structs, cache)
        return nested.size, nested.alignment''',
        new='''    if spelling in structs:
        nested = _layout_struct(spelling, structs, cache)
        return nested.size, 1  # BUG: the nested struct's own alignment is dropped''',
        kind="codegen",
    ),
    Mutation(
        name="mut-emit-layout-array-sized-as-one-element",
        why="an array field's size drops the element-count multiplication -- a uint8_t[2] "
            "padding field (or any future multi-byte array field on the event boundary) is "
            "sized as if it had one element",
        file=EMIT_LAYOUT,
        old='''    m = _ARRAY_RE.match(spelling)
    if m:
        size, align = _size_align(m.group("base"), structs, cache)
        return size * int(m.group("count")), align''',
        new='''    m = _ARRAY_RE.match(spelling)
    if m:
        size, align = _size_align(m.group("base"), structs, cache)
        return size, align  # BUG: element count dropped''',
        kind="codegen",
    ),
    Mutation(
        name="mut-emit-layout-padding-exported-as-field",
        why="a field whose name starts with `_` (padding, by the layout pass's own "
            "convention) is exported into the layout table like any other field",
        file=EMIT_LAYOUT,
        old='''        if not field.name.startswith("_"):
            fields.append(FieldLayout(name, field.name, offset, size))''',
        new='''        fields.append(FieldLayout(name, field.name, offset, size))  # BUG: padding exported''',
        kind="codegen",
    ),

    # ── generated/shipped artifacts, mutated directly ───────────────────
    Mutation(
        name="mut-codon-reject-reason-dropped",
        why="_EV_REJECT_REASON is dropped from the generated codon/flox/layout.codon, "
            "simulating a regenerate that silently lost the one constant the whole feature "
            "exists to add",
        file=LAYOUT_CODON,
        old="_EV_REJECT_REASON = 40\n",
        new="",
        kind="pytest",
        primary_tests=("scripts/tests/test_codon_event_layout.py::"
                       "test_codon_can_read_the_rejection_reason",),
        restore_kind="git-show",
    ),
    Mutation(
        name="mut-codon-dispatch-reads-wrong-offset",
        why="_build_order_event reads reject_reason at _EV_TS_NS instead of "
            "_EV_REJECT_REASON -- a valid, generated constant used at the wrong call site. No "
            "Python-level test reads the *value* at that offset (only that the constant and "
            "the import exist), so this depends on a Codon build to show up",
        file=DISPATCH_CODON,
        old="        _cstr_at(ev, _EV_REJECT_REASON))",
        new="        _cstr_at(ev, _EV_TS_NS))",
        kind="pytest",
        primary_tests=("scripts/tests/test_codon_event_layout.py::"
                       "test_no_binding_file_hardcodes_a_struct_offset",),
        restore_kind="git-show",
    ),

    # ── include/flox/capi/flox_capi_layout.h, mutated directly (C++) ────
    Mutation(
        name="mut-capi-layout-struct-size-wrong",
        why="FloxBar's generated size entry is dropped from 72 to 71 -- offsetof/sizeof "
            "disagree with the table this simulates a drifted generated header",
        file=LAYOUT_HEADER,
        old='{"FloxBar", 72, 8},',
        new='{"FloxBar", 71, 8},',
        kind="cpp",
        restore_kind="git-show",
    ),

    # ── .github/workflows/ci.yml ─────────────────────────────────────────
    Mutation(
        name="mut-ci-pytest-scripts-tests-removed",
        why="the CI step that runs `pytest scripts/tests -q` is removed -- the only thing "
            "holding the two gates honest would run nowhere but a developer's laptop",
        file=CI_YAML,
        old="        python3 -m pytest scripts/tests -q",
        new="        true  # the gate test step was removed",
        primary_tests=("scripts/tests/test_gate_ci_wiring.py::"
                       "test_ci_runs_the_gate_tests",),
    ),
    Mutation(
        name="mut-ci-parity-without-require-quickjs",
        why="the CI parity step drops --require-quickjs -- the flag exists and is never "
            "passed, the original defect this whole task fixes",
        file=CI_YAML,
        old="        python3 scripts/check_binding_parity.py --require-quickjs",
        new="        python3 scripts/check_binding_parity.py",
        primary_tests=("scripts/tests/test_gate_ci_wiring.py::"
                       "test_ci_runs_the_parity_gate_with_quickjs_required",),
    ),
]

# The first mutation's placeholder primary_tests gets fixed up here rather
# than inline, so the literal list above stays readable.
MUTATIONS[0].primary_tests = (
    "scripts/tests/test_binding_parity_gate.py::"
    "test_a_function_dropped_from_the_pybind11_source_turns_the_gate_red",
    "scripts/tests/test_binding_parity_gate.py::"
    "test_a_new_c_function_with_no_wrapper_is_red_in_all_four_bindings",
)


# ── mechanics ────────────────────────────────────────────────────────────


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def apply(path: Path, old: str, new: str, expected: int) -> str:
    text = path.read_text()
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"mutation anchor found {count} time(s), expected {expected}, in "
                         f"{path}:\n  {old!r}")
    mutated = text.replace(old, new)
    if mutated == text:
        raise SystemExit(f"mutation changed nothing in {path}")
    return mutated


def restore(path: Path, rel: str, original: str, before_hash: str, kind: str) -> None:
    if kind == "git-show":
        result = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=REPO,
                                capture_output=True, text=True, check=True)
        path.write_text(result.stdout)
    else:
        path.write_text(original)
    after = sha256(path)
    print(f"  sha256  after   {after}  {rel}")
    if after != before_hash:
        raise SystemExit(f"restore failed: {rel} does not hash back to its original")


def run_pytest(node_ids: list[str], timeout: int) -> tuple[int, str]:
    cmd = [PYTHON, "-m", "pytest", *node_ids, "-q"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"")
        err = (e.stderr or b"")
        if isinstance(out, bytes):
            out = out.decode(errors="replace")
        if isinstance(err, bytes):
            err = err.decode(errors="replace")
        return 124, f"timed out after {timeout}s\n{out}{err}"
    return r.returncode, r.stdout + r.stderr


def summary_line(output: str) -> str:
    tail = [line for line in output.splitlines() if line.strip()]
    return tail[-1].strip() if tail else ""


def run_check_sh(timeout: int) -> tuple[int, str]:
    try:
        r = subprocess.run(["bash", "tools/codegen/scripts/check.sh"],
                           capture_output=True, text=True, timeout=timeout, cwd=REPO)
    except subprocess.TimeoutExpired:
        return 124, f"check.sh timed out after {timeout}s"
    return r.returncode, r.stdout + r.stderr


def object_files(target: str) -> list[Path]:
    out = subprocess.run(
        ["find", str(BUILD), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, output: str):
        super().__init__("rebuild failed")
        self.output = output


def rebuild_cpp(target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(output)
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been a stale "
            f"binary, so the run is refused:\n{output[-2000:]}")
    return output


def run_gtest(target: str, gtest_filter: str | None, timeout: int) -> tuple[int, str]:
    binary = BUILD / "tests" / target
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout}s"
    return r.returncode, r.stdout + r.stderr


# ── control ──────────────────────────────────────────────────────────────


def control() -> bool:
    ok = True

    code, out = run_pytest([FULL_SCRIPTS_TESTS], PYTEST_TIMEOUT)
    print(f"  control  pytest {FULL_SCRIPTS_TESTS:<24} -> exit {code} "
          f"({'green' if code == 0 else 'RED'})   {summary_line(out)}")
    if code != 0:
        print(out[-3000:])
    ok = ok and code == 0

    code, out = run_pytest([FULL_CODEGEN_TESTS], PYTEST_TIMEOUT)
    print(f"  control  pytest {FULL_CODEGEN_TESTS:<24} -> exit {code} "
          f"({'green' if code == 0 else 'RED'})   {summary_line(out)}")
    if code != 0:
        print(out[-3000:])
    ok = ok and code == 0

    code, out = run_check_sh(CHECK_SH_TIMEOUT)
    print(f"  control  tools/codegen/scripts/check.sh -> exit {code} "
          f"({'green' if code == 0 else 'RED'})")
    if code != 0:
        print(out[-3000:])
    ok = ok and code == 0

    for obj in object_files(CPP_TARGET):
        obj.unlink()
    rebuild_cpp(CPP_TARGET)
    code, out = run_gtest(CPP_TARGET, None, TEST_TIMEOUT)
    summary = next((l for l in out.splitlines() if l.startswith("[==========] ")
                    and " ran." in l), "")
    print(f"  control  {CPP_TARGET} (whole binary) -> exit {code} "
          f"({'green' if code == 0 else 'RED'})   {summary.strip()}")
    ok = ok and code == 0

    return ok


# ── sweep ────────────────────────────────────────────────────────────────


def sweep(m: Mutation) -> tuple[bool, str]:
    """Run every other suite named in the task. Returns (caught, detail)."""
    code, out = run_pytest([FULL_SCRIPTS_TESTS], PYTEST_TIMEOUT)
    if code != 0:
        failed = [l for l in out.splitlines() if l.startswith("FAILED ")]
        return True, f"pytest {FULL_SCRIPTS_TESTS}: {failed[:5] or summary_line(out)}"

    code, out = run_pytest([FULL_CODEGEN_TESTS], PYTEST_TIMEOUT)
    if code != 0:
        failed = [l for l in out.splitlines() if l.startswith("FAILED ")]
        return True, f"pytest {FULL_CODEGEN_TESTS}: {failed[:5] or summary_line(out)}"

    code, out = run_check_sh(CHECK_SH_TIMEOUT)
    if code != 0:
        tail = "\n".join(out.splitlines()[-15:])
        return True, f"tools/codegen/scripts/check.sh: {tail}"

    r = subprocess.run([PYTHON, PARITY, "--root", "."], cwd=REPO,
                       capture_output=True, text=True, timeout=PYTEST_TIMEOUT)
    if r.returncode != 0:
        return True, f"check_binding_parity.py --root . on the real tree: exit {r.returncode}"

    code, out = run_gtest(CPP_TARGET, None, TEST_TIMEOUT)
    if code != 0:
        return True, f"{CPP_TARGET} (whole binary): exit {code}"

    return False, ""


# ── per-mutation run ─────────────────────────────────────────────────────


def run_mutation(m: Mutation) -> tuple[str, str]:
    """(verdict, detail). verdict in killed/killed-elsewhere/equivalent/alive/no-compile."""
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  sha256  before  {before}")

    mutated = apply(path, m.old, m.new, m.expected_occurrences)
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")

    verdict = "alive"
    detail = ""
    try:
        if m.kind == "pytest":
            code, out = run_pytest(list(m.primary_tests), PYTEST_TIMEOUT)
            red = code != 0
            print(f"  {', '.join(m.primary_tests)} -> exit {code} "
                  f"({'RED, mutation killed' if red else 'green'})   {summary_line(out)}")
            if red:
                verdict = "killed"
                for line in [l for l in out.splitlines() if l.startswith("FAILED ")][:6]:
                    print(f"    {line}")
            else:
                caught, why = sweep(m)
                if caught:
                    verdict = "killed-elsewhere"
                    detail = why
                    print(f"  sweep -> RED, {detail}")
                else:
                    print("  sweep -> green everywhere")
                    if m.equivalent_reason:
                        verdict = "equivalent"
                        detail = m.equivalent_reason
                    else:
                        verdict = "alive"

        elif m.kind == "codegen":
            code, out = run_check_sh(CHECK_SH_TIMEOUT)
            red = code != 0
            print(f"  tools/codegen/scripts/check.sh -> exit {code} "
                  f"({'RED, mutation killed' if red else 'green'})")
            if red:
                verdict = "killed"
                print("\n".join(out.splitlines()[-20:]))
            else:
                # check.sh already re-ran tools/codegen/tests as its last step;
                # a green check.sh means no drift on the real spec AND the
                # synthetic unit tests agreed. Still run the rest of the sweep
                # (scripts/tests, the real parity gate, the C++ binary) before
                # calling it a hole.
                caught, why = sweep(m)
                if caught:
                    verdict = "killed-elsewhere"
                    detail = why
                    print(f"  sweep -> RED, {detail}")
                else:
                    print("  sweep -> green everywhere")
                    if m.equivalent_reason:
                        verdict = "equivalent"
                        detail = m.equivalent_reason
                    else:
                        verdict = "alive"

        elif m.kind == "cpp":
            removed = object_files(CPP_TARGET)
            for obj in removed:
                obj.unlink()
            print(f"  removed {len(removed)} object file(s) for {CPP_TARGET}")
            try:
                out = rebuild_cpp(CPP_TARGET)
                compiled = sum(1 for l in out.splitlines() if "Building CXX" in l)
                print(f"  rebuilt {CPP_TARGET}: {compiled} 'Building CXX' line(s)")
            except BuildFailed as e:
                print("  DID NOT COMPILE -- not a mutation")
                print("\n".join(e.output.splitlines()[-20:]))
                return "no-compile", "did not compile"

            code, out = run_gtest(CPP_TARGET, None, TEST_TIMEOUT)
            red = code != 0
            print(f"  {CPP_TARGET} (whole binary) -> exit {code} "
                  f"({'RED, mutation killed' if red else 'GREEN, MUTATION SURVIVED'})")
            if red:
                verdict = "killed"
            else:
                caught, why = sweep(m)
                if caught:
                    verdict = "killed-elsewhere"
                    detail = why
                else:
                    verdict = "equivalent" if m.equivalent_reason else "alive"
                    detail = m.equivalent_reason

        else:
            raise SystemExit(f"unknown mutation kind {m.kind!r}")

    finally:
        restore(path, m.file, original, before, m.restore_kind)
        if m.kind == "cpp":
            for obj in object_files(CPP_TARGET):
                obj.unlink()
            rebuild_cpp(CPP_TARGET)

    return verdict, detail


# ── main ─────────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--skip-control", action="store_true")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<44} {m.kind:<8} {m.file}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD} is not configured; run cmake -S . -B build ... first")
    if not (REPO / "tools" / "codegen" / ".venv" / "bin" / "python").is_file():
        raise SystemExit("tools/codegen/.venv is missing; run tools/codegen/setup.sh first")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    if not args.skip_control:
        print("control run before the mutations")
        if not control():
            raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m)) for m in selected]

    restored = True
    if not args.skip_control:
        print("\ncontrol run after the mutations")
        restored = control()

    print("\nsummary")
    label = {"killed": "RED       ", "killed-elsewhere": "RED(else) ", "alive": "ALIVE     ",
             "equivalent": "EQUIVALENT", "no-compile": "NOBUILD   "}
    for m, verdict, detail in results:
        extra = f"  ({detail})" if detail else ""
        print(f"  {label[verdict]}  {m.name:<44} {m.file}{extra}")

    survived = [(m.name, d) for m, v, d in results if v == "alive"]
    equivalent = [(m.name, d) for m, v, d in results if v == "equivalent"]
    killed_elsewhere = [(m.name, d) for m, v, d in results if v == "killed-elsewhere"]
    nobuild = [m.name for m, v, d in results if v == "no-compile"]

    if survived:
        print(f"\n{len(survived)} mutation(s) survived -- holes in the tests:")
        for name, _ in survived:
            print(f"  - {name}")
    if equivalent:
        print(f"\n{len(equivalent)} mutation(s) survived but are equivalent:")
        for name, reason in equivalent:
            print(f"  - {name}: {reason}")
    if killed_elsewhere:
        print(f"\n{len(killed_elsewhere)} mutation(s) killed by the sweep, not the primary "
              f"test:")
        for name, reason in killed_elsewhere:
            print(f"  - {name}: {reason}")
    if nobuild:
        print(f"\n{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")

    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
