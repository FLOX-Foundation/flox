#!/usr/bin/env python3
"""Mutation harness for the presence flags on FloxBookSnapshot.

`has_bid` and `has_ask` say whether a side of the book has a best level; the
price fields cannot, because a book may be quoted at exactly zero. The flags
are written in two places, `BridgeStrategy::toBookSnapshot` (the book event
and the context handed to callbacks) and `flox_get_symbol_context` (the query
outside a callback), and the ABI number moved to 4 with the struct.

Each mutation below breaks one of those writes, or the number, or the
binding that reads the flags (Python None, Node null, QuickJS null, the Codon
context on the _opt accessors), rebuilds what reads it, and requires the
tests to go red. A test that passes proves nothing on its own; what it has
to do is fail when the code under it is wrong.

Run from the repository root with a configured `build/` (FLOX_BUILD_CAPI=ON,
FLOX_BUILD_TESTS=ON, FLOX_BUILD_QUICKJS=ON), a `build-py/` with
FLOX_BUILD_PYTHON=ON, and the Node addon built with `npx cmake-js compile`
under `node/`:

    python3 scripts/mutations/book_presence.py
"""

from __future__ import annotations

import glob
import hashlib
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"

BRIDGE = "include/flox/capi/bridge_strategy.h"
SHIM = "src/capi/flox_capi.cpp"
HEADER = "include/flox/capi/flox_capi.h"

PY_BINDING = "python/strategy_bindings.h"
NODE_BINDING = "node/src/strategy.h"
QJS_BINDING = "src/quickjs/js_strategy.cpp"
CODON_CONTEXT = "codon/flox/context.codon"

TARGETS = ["test_capi_book_snapshot", "test_capi_negative_book", "test_capi_infra"]

# (kind, target): "gtest" is a target under build/tests, "py" a pytest file
# run against build-py, "node" a script under node/test, "text" a pytest file
# that reads sources and needs no build.
PY_TEST = ("py", "python/tests/test_symbol_context_quotes.py")
NODE_TEST = ("node", "test/test_symbol_context_quotes.js")
QJS_TEST = ("gtest", "test_quickjs")
CODON_TEST = ("text", "python/tests/test_codon_context_quotes.py")

MUTATIONS = [
    (
        "bridge: has_bid never set",
        BRIDGE,
        "      snap.has_bid = 1;\n",
        "      snap.has_bid = 0;\n",
        ["test_capi_book_snapshot"],
    ),
    (
        "bridge: has_ask never set",
        BRIDGE,
        "      snap.has_ask = 1;\n",
        "      snap.has_ask = 0;\n",
        ["test_capi_book_snapshot"],
    ),
    (
        "bridge: has_ask reports the bid side",
        BRIDGE,
        "    if (ask)\n    {\n      snap.has_ask = 1;\n",
        "    if (ask)\n    {\n      snap.has_ask = bid ? 1 : 0;\n",
        ["test_capi_book_snapshot"],
    ),
    (
        "shim: has_bid from the price, so a bid at zero reads as absent",
        SHIM,
        "  out->book.has_bid = bid ? 1u : 0u;\n",
        "  out->book.has_bid = (bid && bid->raw() != 0) ? 1u : 0u;\n",
        ["test_capi_negative_book"],
    ),
    (
        "shim: has_ask always set",
        SHIM,
        "  out->book.has_ask = ask ? 1u : 0u;\n",
        "  out->book.has_ask = 1u;\n",
        ["test_capi_book_snapshot", "test_capi_negative_book"],
    ),
    (
        "shim: the flags swap sides",
        SHIM,
        "  out->book.has_bid = bid ? 1u : 0u;\n  out->book.has_ask = ask ? 1u : 0u;\n",
        "  out->book.has_bid = ask ? 1u : 0u;\n  out->book.has_ask = bid ? 1u : 0u;\n",
        ["test_capi_negative_book"],
    ),
    (
        "header: the ABI number stays at 3 with the flags present",
        HEADER,
        "#define FLOX_CAPI_ABI_VERSION 4\n",
        "#define FLOX_CAPI_ABI_VERSION 3\n",
        ["test_capi_infra"],
    ),
    (
        "python: a bid at zero reads as no quote",
        PY_BINDING,
        "  if (book.has_bid)\n  {\n    pc.best_bid = flox_price_to_double(book.bid_price_raw);\n",
        "  if (book.has_bid && book.bid_price_raw != 0)\n  {\n    pc.best_bid = flox_price_to_double(book.bid_price_raw);\n",
        [PY_TEST],
    ),
    (
        "python: the mid needs only a bid",
        PY_BINDING,
        "  if (book.has_bid && book.has_ask)\n  {\n    pc.mid_price = flox_price_to_double(book.mid_raw);\n",
        "  if (book.has_bid)\n  {\n    pc.mid_price = flox_price_to_double(book.mid_raw);\n",
        [PY_TEST],
    ),
    (
        "python: the spread answers 0.0 for a missing side",
        PY_BINDING,
        "      return *best_ask - *best_bid;\n    }\n    return std::nullopt;\n",
        "      return *best_ask - *best_bid;\n    }\n    return 0.0;\n",
        [PY_TEST],
    ),
    (
        "python: the strategy accessor reads the price, not the flag",
        PY_BINDING,
        "    return bid ? std::optional<double>(bid->toDouble()) : std::nullopt;\n",
        "    return (bid && bid->raw() != 0) ? std::optional<double>(bid->toDouble()) : std::nullopt;\n",
        [PY_TEST],
    ),
    (
        "node: the ask flag is read from the bid side",
        NODE_BINDING,
        "    o.Set(\"bestAsk\", ctx->book.has_ask\n",
        "    o.Set(\"bestAsk\", ctx->book.has_bid\n",
        [NODE_TEST],
    ),
    (
        "node: the mid needs only a bid",
        NODE_BINDING,
        "    const bool both = ctx->book.has_bid && ctx->book.has_ask;\n",
        "    const bool both = ctx->book.has_bid;\n",
        [NODE_TEST],
    ),
    (
        "quickjs: the spread needs only an ask",
        QJS_BINDING,
        "  const bool both = snap->has_bid && snap->has_ask;\n",
        "  const bool both = snap->has_ask;\n",
        [QJS_TEST],
    ),
    (
        "quickjs: a bid at zero reads as null",
        QJS_BINDING,
        "                    snap->has_bid ? JS_NewFloat64(c, flox_price_to_double(snap->bid_price_raw))\n",
        "                    (snap->has_bid && snap->bid_price_raw != 0) ? JS_NewFloat64(c, flox_price_to_double(snap->bid_price_raw))\n",
        [QJS_TEST],
    ),
    (
        "codon: the context goes back to the raw bid accessor",
        CODON_CONTEXT,
        "        if flox_best_bid_raw_opt(self._handle, self.symbol_id, __ptr__(raw)) == u8(0):\n            return None\n",
        "        if flox_best_bid_raw(self._handle, self.symbol_id) == i64(0):\n            return None\n",
        [CODON_TEST],
    ),
    (
        "codon: the spread guards on a positive price again",
        CODON_CONTEXT,
        "        if bid is None or ask is None:\n            return None\n        return ask - bid\n",
        "        if bid is None or ask is None or bid <= 0.0 or ask <= 0.0:\n            return None\n        return ask - bid\n",
        [CODON_TEST],
    ),
]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def as_pair(t):
    return t if isinstance(t, tuple) else ("gtest", t)


def rebuild(targets: list) -> tuple[bool, bool]:
    ok, built = True, False
    kinds = {k for k, _ in map(as_pair, targets)}
    gtests = [t for k, t in map(as_pair, targets) if k == "gtest"]
    if gtests:
        for t in gtests:
            for o in glob.glob(str(BUILD / "**" / "CMakeFiles" / f"{t}.dir" / "**" / "*.o"), recursive=True):
                os.remove(o)
        r = subprocess.run(
            ["cmake", "--build", str(BUILD), "-j4", "--target", "flox_capi", *gtests],
            cwd=REPO, capture_output=True, text=True, timeout=3600,
        )
        ok &= r.returncode == 0
        built |= "Building CXX" in r.stdout
    if "py" in kinds:
        for o in glob.glob(str(REPO / "build-py" / "**" / "flox_py.cpp.o"), recursive=True):
            os.remove(o)
        r = subprocess.run(
            ["cmake", "--build", str(REPO / "build-py"), "-j4", "--target", "_flox_py"],
            cwd=REPO, capture_output=True, text=True, timeout=3600,
        )
        ok &= r.returncode == 0
        built |= "Building CXX" in r.stdout
    if "node" in kinds:
        for o in glob.glob(str(REPO / "node" / "build" / "**" / "*strategy*.o"), recursive=True):
            os.remove(o)
        r = subprocess.run(
            ["npx", "cmake-js", "compile"], cwd=REPO / "node", capture_output=True, text=True, timeout=3600,
        )
        ok &= r.returncode == 0
        built |= "Building CXX" in r.stdout or "Linking" in r.stdout
    if "text" in kinds:
        built = True
    return ok, built


def run(target) -> str:
    kind, name = as_pair(target)
    env = dict(os.environ, FLOX_REPO_ROOT=str(REPO))
    if kind == "gtest":
        exe = BUILD / "tests" / name
        if not exe.exists():
            return "NOBIN"
        cmd, cwd = [str(exe)], REPO
    elif kind == "py":
        env["PYTHONPATH"] = str(REPO / "build-py" / "python")
        cmd, cwd = [sys.executable, "-m", "pytest", name, "-q", "-p", "no:cacheprovider"], REPO
    elif kind == "node":
        cmd, cwd = ["node", name], REPO / "node"
    else:
        cmd, cwd = [sys.executable, "-m", "pytest", name, "-q", "-p", "no:cacheprovider"], REPO
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=900, env=env)
    return "GREEN" if r.returncode == 0 else "RED"


def main() -> int:
    rows = []
    for name, rel, old, new, targets in MUTATIONS:
        path = REPO / rel
        src = path.read_text()
        before = sha(path)
        if src.count(old) != 1:
            rows.append((name, f"ANCHOR x{src.count(old)}"))
            print(rows[-1], flush=True)
            continue
        path.write_text(src.replace(old, new))
        try:
            ok, built = rebuild(targets)
            if not ok:
                result = "NOCOMPILE"
            else:
                result = " / ".join(f"{as_pair(t)[1]}:{run(t)}" for t in targets)
                if not built:
                    result += " (not rebuilt)"
        finally:
            path.write_text(src)
            assert sha(path) == before
        rows.append((name, result))
        print(f"{name:<64} {result}", flush=True)

    controls = [*TARGETS, PY_TEST, NODE_TEST, QJS_TEST, CODON_TEST]
    ok, _ = rebuild(controls)
    control = " / ".join(f"{as_pair(t)[1]}:{run(t)}" for t in controls) if ok else "NOCOMPILE"
    rows.append(("control (restored)", control))

    print("\n=== TABLE ===")
    for name, result in rows:
        print(f"{name:<64} {result}")
    killed = sum(1 for _, r in rows[:-1] if "RED" in r)
    print(f"\n{killed}/{len(MUTATIONS)} mutations red; control: {control}")
    return 0 if killed == len(MUTATIONS) and "RED" not in control else 1


if __name__ == "__main__":
    sys.exit(main())
