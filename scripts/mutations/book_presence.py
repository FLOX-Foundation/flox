#!/usr/bin/env python3
"""Mutation harness for the presence flags on FloxBookSnapshot.

`has_bid` and `has_ask` say whether a side of the book has a best level; the
price fields cannot, because a book may be quoted at exactly zero. The flags
are written in two places, `BridgeStrategy::toBookSnapshot` (the book event
and the context handed to callbacks) and `flox_get_symbol_context` (the query
outside a callback), and the ABI number moved to 4 with the struct.

Each mutation below breaks one of those writes, or the number, rebuilds the
test targets that read them, and requires the tests to go red. A test that
passes proves nothing on its own; what it has to do is fail when the code
under it is wrong.

Run from the repository root with a configured `build/` (FLOX_BUILD_CAPI=ON,
FLOX_BUILD_TESTS=ON):

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

TARGETS = ["test_capi_book_snapshot", "test_capi_negative_book", "test_capi_infra"]

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
]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rebuild(targets: list[str]) -> tuple[bool, bool]:
    for t in targets:
        for o in glob.glob(str(BUILD / "**" / "CMakeFiles" / f"{t}.dir" / "**" / "*.o"), recursive=True):
            os.remove(o)
    r = subprocess.run(
        ["cmake", "--build", str(BUILD), "-j4", "--target", "flox_capi", *targets],
        cwd=REPO,
        capture_output=True,
        text=True,
        timeout=3600,
    )
    return r.returncode == 0, "Building CXX" in r.stdout


def run(target: str) -> str:
    exe = BUILD / "tests" / target
    if not exe.exists():
        return "NOBIN"
    r = subprocess.run(
        [str(exe)], cwd=REPO, capture_output=True, text=True, timeout=600,
        env=dict(os.environ, FLOX_REPO_ROOT=str(REPO)),
    )
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
                result = " / ".join(f"{t}:{run(t)}" for t in targets)
                if not built:
                    result += " (not rebuilt)"
        finally:
            path.write_text(src)
            assert sha(path) == before
        rows.append((name, result))
        print(f"{name:<64} {result}", flush=True)

    ok, _ = rebuild(TARGETS)
    control = " / ".join(f"{t}:{run(t)}" for t in TARGETS) if ok else "NOCOMPILE"
    rows.append(("control (restored)", control))

    print("\n=== TABLE ===")
    for name, result in rows:
        print(f"{name:<64} {result}")
    killed = sum(1 for _, r in rows[:-1] if "RED" in r)
    print(f"\n{killed}/{len(MUTATIONS)} mutations red; control: {control}")
    return 0 if killed == len(MUTATIONS) and "RED" not in control else 1


if __name__ == "__main__":
    sys.exit(main())
