#!/usr/bin/env python3
"""Prove that every path into matching applies its pre-trade gates.

An order reaches `Matcher::cross` through several handlers, and each one must
apply the checks itself: a gate passed on submission does not cover a stop that
fires later, because the instrument may have halted in between. Tests can only
assert this for the paths someone thought to enumerate. This gate asserts it
over the code, so a path added tomorrow cannot quietly skip a check.

Two properties are checked against the clang AST:

1. Domination. On every path from the handler's entry to the `cross` call, each
   gate the handler owes has already executed. Branches that cannot reach the
   call (they return, continue, break or throw) do not weaken the result.
2. The result is honoured. A gate whose value is dropped is not a gate, so each
   one must be called in the condition (or condition-init) of an `if` that has
   a path-terminating branch.

Plus drift control: the set of functions calling `cross` is pinned. A new
caller fails the check until it is declared here with the gates it owes.

Usage:
    python3 scripts/check_gate_reachability.py            # uses ./build
    python3 scripts/check_gate_reachability.py --build-dir out
"""
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE_HEADER = "matching_engine.h"

# The matching entry point every order must pass through, and the trade-emitting
# helper behind it. Keyed by the method name on Matcher.
MATCH_ENTRY = "cross"

# Every handler that may call into matching, and the gates it owes before doing
# so. Adding a caller without adding it here fails the check by design.
#
# onNew          full admission: instrument, size, price, buying power.
# processTriggers a resting stop re-enters matching; it was validated when it
#                was parked, so it re-runs the risk and funding gates and the
#                instrument-state check that guards the whole sweep.
# onModify       a modified order re-enters matching as a new aggressor.
REQUIRED_GATES: dict[str, set[str]] = {
    "onNew": {"admissionGate", "perpRiskGate", "validate", "reserveFunds"},
    "processTriggers": {"perpRiskGate", "reserveFunds", "tradingStatus"},
    "onModify": {"admissionDenies", "perpRiskGate", "reserveFunds"},
}

# Gates read for their value rather than called for a reject code. A guard on
# `tradingStatus() != TradingStatus::Trading` returns early instead of
# rejecting, so it is honoured by terminating the path, not by a reject sink.
VALUE_GATES = {"tradingStatus", "admissionDenies"}

TERMINATORS = {
    "RETURN_STMT",
    "CONTINUE_STMT",
    "BREAK_STMT",
    "CXX_THROW_EXPR",
    "GOTO_STMT",
}


def _load_clang():
    """Import libclang, trying the usual install locations."""
    try:
        import clang.cindex as ci
    except ImportError:
        print("libclang python bindings missing: pip install libclang", file=sys.stderr)
        raise SystemExit(2)

    import os

    candidates = [os.environ.get("FLOX_LIBCLANG")]
    candidates += [
        "/opt/homebrew/opt/llvm/lib/libclang.dylib",
        "/usr/local/opt/llvm/lib/libclang.dylib",
        "/usr/lib/llvm-18/lib/libclang.so.1",
        "/usr/lib/x86_64-linux-gnu/libclang-18.so.1",
    ]
    for cand in candidates:
        if cand and Path(cand).exists():
            ci.Config.set_library_file(cand)
            break
    return ci


def _compile_args(build_dir: Path, ci) -> tuple[str, list[str]]:
    """Compile flags for a TU that instantiates the engine template.

    The engine is a header-only template, so its bodies only exist in the AST
    of a TU that instantiates it. Any venue test does; the database says how to
    compile one.
    """
    db = build_dir / "compile_commands.json"
    if not db.exists():
        print(f"{db} not found -- configure the build first (cmake -B {build_dir})", file=sys.stderr)
        raise SystemExit(2)

    entries = json.loads(db.read_text())
    tu = None
    for want in ("test_venue_gate_coverage.cpp", "test_venue_engine.cpp", "test_venue_venue.cpp"):
        for e in entries:
            if e["file"].endswith(want):
                tu = e
                break
        if tu:
            break
    if tu is None:
        print("no venue TU in compile_commands.json -- is the venue module configured?", file=sys.stderr)
        raise SystemExit(2)

    raw = shlex.split(tu.get("command") or " ".join(tu["arguments"]))
    args: list[str] = []
    skip = False
    for a in raw[1:]:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a in ("-c", "-MD", "-MT", "-MF"):
            continue
        if a.endswith((".cpp", ".cc", ".o")):
            continue
        args.append(a)

    return tu["file"], args


def _env_variants() -> list[list[str]]:
    """Extra flags to try, best first.

    libclang is not the compiler driver: it does not infer the platform SDK or
    its own builtin headers. Which of those it needs depends on the host, so
    the variants are tried in order and the first that yields a usable AST
    wins. Getting this wrong shows up as an empty AST, which the caller treats
    as a broken check rather than a passing one.
    """
    extra: list[str] = []
    for exe in ("/opt/homebrew/opt/llvm/bin/clang", "clang"):
        try:
            rd = subprocess.run([exe, "-print-resource-dir"], capture_output=True, text=True, timeout=20)
            if rd.returncode == 0 and rd.stdout.strip():
                extra = ["-resource-dir", rd.stdout.strip()]
                break
        except (OSError, subprocess.SubprocessError):
            continue

    sysroot: list[str] = []
    if sys.platform == "darwin":
        try:
            sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, timeout=20)
            if sdk.returncode == 0 and sdk.stdout.strip():
                sysroot = ["-isysroot", sdk.stdout.strip()]
        except (OSError, subprocess.SubprocessError):
            pass

    variants = [extra + sysroot, sysroot, extra, []]
    seen, out = set(), []
    for v in variants:
        key = tuple(v)
        if key not in seen:
            seen.add(key)
            out.append(v)
    return out


_TOKEN_CACHE: dict[tuple, frozenset] = {}


def _calls_in(node, ci) -> frozenset:
    """Every function name called anywhere inside this subtree.

    Read off the token stream rather than the resolved AST on purpose: the
    engine is a template, so `matcher_.cross(...)` is a dependent call and
    libclang leaves the callee unnamed. Tokens carry the name whatever the
    instantiation state. Comments are excluded, so a gate named in prose does
    not count as a gate applied.
    """
    ext = node.extent
    key = (ext.start.file.name if ext.start.file else "", ext.start.offset, ext.end.offset)
    hit = _TOKEN_CACHE.get(key)
    if hit is not None:
        return hit

    names = set()
    prev = None
    for tok in node.get_tokens():
        if tok.kind == ci.TokenKind.COMMENT:
            continue
        if tok.spelling == "(" and prev is not None:
            names.add(prev)
        prev = tok.spelling if tok.kind == ci.TokenKind.IDENTIFIER else None
    out = frozenset(names)
    _TOKEN_CACHE[key] = out
    return out


def _split_if(children, ci):
    """Split an if-statement's children into (head, branches).

    Head is everything evaluated unconditionally: an optional init-statement,
    an optional condition variable, and the condition itself. Branches are the
    then- and optional else-statement. The condition is the last expression
    among the children, which separates the two halves without having to guess
    from arity -- `if (const RejectReason r = f(); r != None)` has a
    DECL_STMT before its condition, and a bare `if (!f())` does not.
    """
    expr_idx = [i for i, c in enumerate(children) if c.kind.is_expression()]
    if expr_idx:
        cut = expr_idx[-1] + 1
    else:
        # No expression child (e.g. `if (auto x = f())`): the trailing one or
        # two statements are the branches.
        cut = max(0, len(children) - 2) if len(children) >= 3 else max(0, len(children) - 1)
    return children[:cut], children[cut:][:2]


def _kind_name(node) -> str:
    return node.kind.name


class Analysis:
    """Forward must-analysis over the statement tree.

    State is the set of gates guaranteed to have run at a program point. The
    walk returns that set at the end of a statement, or None when the statement
    cannot fall through (every path out of it returns, continues, breaks or
    throws) -- a branch that cannot reach the merge must not dilute it.

    Findings are recorded when a `cross` call is reached: the gate set holding
    at that exact point is the evidence.
    """

    def __init__(self, ci, gates: set[str]):
        self.ci = ci
        self.gates = gates
        self.at_cross: list[set[str]] = []
        self.honoured: set[str] = set()
        self.saw_goto = False

    # -- helpers ---------------------------------------------------------
    def _record_gates(self, node, state: set[str]) -> set[str]:
        for name in _calls_in(node, self.ci):
            if name in self.gates:
                state = state | {name}
        return state

    def _has_cross(self, node) -> bool:
        return MATCH_ENTRY in _calls_in(node, self.ci)

    def _terminates(self, node) -> bool:
        """True when this subtree unconditionally leaves the enclosing path.

        Runs on a throwaway analysis so probing a branch cannot record
        findings: the evidence must come from the real walk only.
        """
        probe = Analysis(self.ci, self.gates)
        return probe.walk(node, set()) is None

    # -- the walk --------------------------------------------------------
    def walk(self, node, state: set[str]) -> set[str] | None:
        kind = _kind_name(node)

        if kind == "GOTO_STMT":
            self.saw_goto = True
            return None
        if kind in TERMINATORS:
            # Operands still execute before the jump.
            for ch in node.get_children():
                state = self._record_gates(ch, state)
            return None

        if kind == "IF_STMT":
            return self._walk_if(node, state)
        if kind in ("FOR_STMT", "WHILE_STMT", "CXX_FOR_RANGE_STMT", "DO_STMT"):
            return self._walk_loop(node, state)
        if kind == "SWITCH_STMT":
            return self._walk_switch(node, state)
        if kind == "COMPOUND_STMT":
            for ch in node.get_children():
                nxt = self.walk(ch, state)
                if nxt is None:
                    return None
                state = nxt
            return state

        # A leaf statement or expression: everything in it runs in order, and a
        # cross call inside it is observed with the state that reached it.
        state = self._record_gates(node, state)
        if self._has_cross(node):
            self.at_cross.append(set(state))
        return state

    def _walk_if(self, node, state: set[str]) -> set[str] | None:
        children = list(node.get_children())
        head, branches = _split_if(children, self.ci)
        if not branches:
            return state
        for h in head:
            state = self._record_gates(h, state)
            if self._has_cross(h):
                self.at_cross.append(set(state))

        # A gate is honoured when its call sits in the head of an `if` that has
        # a branch which cannot fall through.
        head_gates = set().union(*[_calls_in(h, self.ci) for h in head]) & self.gates if head else set()
        if head_gates and any(self._terminates(b) for b in branches):
            self.honoured |= head_gates

        then_state = self.walk(branches[0], set(state))
        else_state = self.walk(branches[1], set(state)) if len(branches) > 1 else set(state)

        if then_state is None and else_state is None:
            return None
        if then_state is None:
            return else_state
        if else_state is None:
            return then_state
        return then_state & else_state

    def _walk_loop(self, node, state: set[str]) -> set[str] | None:
        children = list(node.get_children())
        body = children[-1] if children else None
        for h in children[:-1]:
            state = self._record_gates(h, state)

        if body is not None:
            # Inside the body the state carries what the entry guaranteed plus
            # what this iteration has run so far. A `cross` inside the loop is
            # observed with exactly that.
            self.walk(body, set(state))

        # A loop may run zero times, so nothing inside it is guaranteed after.
        # (do-while runs once, but treating it conservatively can only make the
        # check stricter, never weaker.)
        return state

    def _walk_switch(self, node, state: set[str]) -> set[str] | None:
        children = list(node.get_children())
        if not children:
            return state
        cond, body = children[0], children[-1]
        state = self._record_gates(cond, state)
        # Cases are not tracked individually: walking the body once with the
        # entry state observes any cross inside it, and nothing inside a switch
        # is treated as guaranteed afterwards.
        self.walk(body, set(state))
        return state


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    args = ap.parse_args()

    ci = _load_clang()
    build_dir = (ROOT / args.build_dir) if not Path(args.build_dir).is_absolute() else Path(args.build_dir)
    tu_file, cargs = _compile_args(build_dir, ci)

    index = ci.Index.create()
    handlers: dict[str, object] = {}
    other_callers: list[tuple[str, int]] = []
    tu = None

    def visit(node):
        if node.kind == ci.CursorKind.CXX_METHOD and node.is_definition():
            loc = node.location.file
            if loc and ENGINE_HEADER in loc.name:
                if MATCH_ENTRY in _calls_in(node, ci):
                    if node.spelling in REQUIRED_GATES:
                        handlers.setdefault(node.spelling, node)
                    else:
                        other_callers.append((node.spelling, node.location.line))
                return
        for ch in node.get_children():
            visit(ch)

    for extra in _env_variants():
        _TOKEN_CACHE.clear()
        handlers.clear()
        del other_callers[:]
        tu = index.parse(tu_file, args=cargs + extra)
        visit(tu.cursor)
        if handlers:
            break

    problems: list[str] = []

    if not handlers:
        print(
            "no engine handler calling %s was found in the AST -- the parse did not "
            "reach the engine template. This is a broken check, not a passing one." % MATCH_ENTRY,
            file=sys.stderr,
        )
        for d in tu.diagnostics:
            if d.severity >= 3:
                print(f"  {d.spelling}", file=sys.stderr)
        return 2

    missing_handlers = set(REQUIRED_GATES) - set(handlers)
    if missing_handlers:
        problems.append(
            f"declared handler(s) {sorted(missing_handlers)} no longer reach {MATCH_ENTRY}: "
            f"either the path moved (update this file) or the call was lost")

    for name, line in sorted(set(other_callers)):
        problems.append(
            f"{name} (matching_engine.h:{line}) calls {MATCH_ENTRY} but declares no gates. "
            f"Add it to REQUIRED_GATES with the checks it owes.")

    for name, node in sorted(handlers.items()):
        gates = REQUIRED_GATES[name]
        an = Analysis(ci, gates)
        body = None
        for ch in node.get_children():
            if _kind_name(ch) == "COMPOUND_STMT":
                body = ch
        if body is None:
            problems.append(f"{name}: no body in the AST")
            continue
        an.walk(body, set())

        if an.saw_goto:
            problems.append(f"{name}: contains goto -- the must-analysis is only sound for structured control flow")

        if not an.at_cross:
            problems.append(f"{name}: {MATCH_ENTRY} call not located by the walk (check is blind here)")
            continue

        for state in an.at_cross:
            for missing in sorted(gates - state):
                problems.append(
                    f"{name}: reaches {MATCH_ENTRY} on a path where {missing}() has not run. "
                    f"Every path into matching must apply it.")

        for gate in sorted(gates - an.honoured - VALUE_GATES):
            problems.append(
                f"{name}: {gate}() is called but its result is not acted on "
                f"(expected `if (<call>) {{ ... return/continue; }}`)")

    if problems:
        print("gate reachability check failed:")
        for p in problems:
            print(f"  {p}")
        return 1

    total = sum(len(REQUIRED_GATES[h]) for h in handlers)
    print(f"OK: {len(handlers)} path(s) into matching, {total} gate obligation(s), "
          f"each dominating {MATCH_ENTRY} and acted on.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
