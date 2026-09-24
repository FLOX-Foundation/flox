#!/usr/bin/env python3
"""Mutation harness for W33-T001 (order-entry enums off the wire).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each fix in W33-T001 one at a
time, in the source, and checks that the test written for it goes red -- and
that an unmutated tree goes green before and after.

Every run is honest about the build: the mutated file's hash is printed before
and after, the target's object files are deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" or the run is refused,
and the test binary runs under a timeout. A mutation that does not compile is
not a mutation, and the run stops rather than reporting one.

Usage:

    python3 scripts/mutations/w33_t001.py            # control, all mutations, control
    python3 scripts/mutations/w33_t001.py --list
    python3 scripts/mutations/w33_t001.py --only decoder-stp-range

The build directory is expected to be configured already:

    cmake --preset venue-lite
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build-venue-lite"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
# Longer than the in-test deadline on the auction repro: with the uncross
# guard removed the loop never returns, and the test's own 10s wait is what
# has to report it, not this timeout.
TEST_TIMEOUT = 120

CODEC = "venue/include/flox-venue/sbe_order_entry_codec.h"
CREDIT = "venue/include/flox-venue/engine/credit.h"
VALIDATE = "venue/include/flox-venue/engine/validate.inl"
SESSION = "venue/include/flox-venue/engine/session.inl"

RANGE = "test_venue_wire_enum_range"
FUZZ = "test_venue_parser_fuzz"


@dataclass
class Mutation:
    name: str
    why: str
    file: str
    old: str
    new: str
    target: str
    test: str  # gtest filter


def decoder_check(field: str, cast: str, byte: int, message: str) -> str:
    return (
        f"        if (!inRange(static_cast<{cast}>(b[{byte}])))\n"
        "        {\n"
        f'          err = "{message}";\n'
        "          return std::nullopt;\n"
        "        }\n"
    )


MUTATIONS = [
    Mutation(
        name="decoder-side-range",
        why="the decoder takes the side byte as given",
        file=CODEC,
        old=decoder_check("side", "Side", 12,
                          "EnterOrder: side is not a value this schema defines"),
        new="",
        target=RANGE,
        test="WireEnumRange.AnOutOfRangeSideIsRefusedWithAReason",
    ),
    Mutation(
        name="decoder-type-range",
        why="the decoder takes the order-type byte as given",
        file=CODEC,
        old=decoder_check("type", "OrderType", 13,
                          "EnterOrder: order type is not a value this schema defines"),
        new="",
        target=RANGE,
        test="WireEnumRange.AnOutOfRangeOrderTypeIsRefusedWithAReason",
    ),
    Mutation(
        name="decoder-tif-range",
        why="the decoder takes the time-in-force byte as given",
        file=CODEC,
        old=decoder_check("tif", "TimeInForce", 14,
                          "EnterOrder: tif is not a value this schema defines"),
        new="",
        target=RANGE,
        test="WireEnumRange.AnOutOfRangeTimeInForceIsRefusedWithAReason",
    ),
    Mutation(
        name="decoder-stp-range",
        why="the decoder takes the self-trade-mode byte as given -- the byte that hung the uncross",
        file=CODEC,
        old=decoder_check("stp", "STPMode", 16,
                          "EnterOrder: stp mode is not a value this schema defines"),
        new="",
        target=RANGE,
        test="WireEnumRange.AnOutOfRangeStpModeIsRefusedWithAReason",
    ),
    Mutation(
        # The same removal, seen by the fuzz instead. Finding 27 was that the
        # fuzz discarded everything it decoded, so this is the mutation that
        # says whether its new assertions bite at all.
        name="decoder-stp-range-fuzz",
        why="the same removal, this time against the parser fuzz's own assertions",
        file=CODEC,
        old=decoder_check("stp", "STPMode", 16,
                          "EnterOrder: stp mode is not a value this schema defines"),
        new="",
        target=FUZZ,
        test="ParserFuzz.EngineSuite",
    ),
    Mutation(
        name="decoder-peg-range",
        why="the decoder takes the peg-reference byte as given",
        file=CODEC,
        old=decoder_check("peg", "PegRef", 75,
                          "EnterOrder: peg reference is not a value this schema defines"),
        new="",
        target=RANGE,
        test="WireEnumRange.AnOutOfRangePegRefIsRefusedWithAReason",
    ),
    Mutation(
        name="decoder-ladder-stp-range",
        why="a ladder's self-trade mode reaches sixteen orders unchecked",
        file=CODEC,
        old=decoder_check("stp", "STPMode", 53,
                          "QuoteLadder: stp mode is not a value this schema defines"),
        new="",
        target=RANGE,
        test="WireEnumRange.AQuoteLadderEnumIsCheckedToo",
    ),
    Mutation(
        name="decoder-ladder-tif-range",
        why="a ladder's time in force reaches sixteen orders unchecked",
        file=CODEC,
        old=decoder_check("tif", "TimeInForce", 57,
                          "QuoteLadder: tif is not a value this schema defines"),
        new="",
        target=RANGE,
        test="WireEnumRange.AQuoteLadderEnumIsCheckedToo",
    ),
    Mutation(
        name="admission-type-shift",
        why="admissionGate shifts the type bitmap by an unchecked type again",
        file=CREDIT,
        old="        (!inRange(o.type) || (p.allowedTypes & (1u << static_cast<uint32_t>(o.type))) == 0))",
        new="        ((p.allowedTypes & (1u << static_cast<uint32_t>(o.type))) == 0))",
        target=RANGE,
        test="WireEnumRange.AnOrderTypePastTheProfileBitmapIsNotPermitted",
    ),
    Mutation(
        name="admission-tif-shift",
        why="admissionGate shifts the tif bitmap by an unchecked time in force again",
        file=CREDIT,
        old="        (!inRange(o.tif) || (p.allowedTif & (1u << static_cast<uint32_t>(o.tif))) == 0))",
        new="        ((p.allowedTif & (1u << static_cast<uint32_t>(o.tif))) == 0))",
        target=RANGE,
        test="WireEnumRange.ATimeInForcePastTheProfileBitmapIsNotPermitted",
    ),
    Mutation(
        name="validate-unknown-type",
        why="an order type the venue cannot name skips the LIMIT-keyed price block again",
        file=VALIDATE,
        old="  if (!inRange(o.type))\n  {\n    return RejectReason::UnknownOrderType;\n  }\n",
        new="",
        target=RANGE,
        test="WireEnumRange.AnUnknownOrderTypeDoesNotRestAtAnUncheckedPrice",
    ),
    Mutation(
        name="uncross-no-progress",
        why="the auction uncross re-peeks a pair its verdict never acted on",
        file=SESSION,
        old=("        if (!v.decrement && !v.cancelBid && !v.cancelAsk)\n"
             "        {\n"
             "          cancelForStp(bidId, bAcct);\n"
             "          cancelForStp(askId, aAcct);\n"
             "          continue;\n"
             "        }\n"),
        new="",
        target=RANGE,
        test="WireEnumRange.TheUncrossTerminatesOnAVerdictThatNamesNoAction",
    ),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def object_files(target: str) -> list[Path]:
    """The target's own object files, located the way `find` would."""
    out = subprocess.run(
        ["find", str(BUILD), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


def rebuild(target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"rebuild of {target} failed:\n{output[-4000:]}")
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    cmd = [str(BUILD / "venue" / target)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        # Deleted first so the control binary is compiled from the source as it
        # stands right now, not served from whatever the last run left behind.
        for obj in object_files(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target, None)
        state = "green" if code == 0 else "RED"
        summary = next((line for line in output.splitlines()
                        if line.startswith("[==========] ") and " ran." in line), "")
        print(f"  control {target:<32} {state}   {summary.strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> bool:
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  sha256  before  {before}")

    if original.count(m.old) != 1:
        raise SystemExit(
            f"mutation {m.name}: its `old` text appears {original.count(m.old)} times in "
            f"{m.file}, expected exactly one"
        )
    path.write_text(original.replace(m.old, m.new, 1))
    print(f"  sha256  mutated {sha256(path)}")

    try:
        removed = object_files(m.target)
        for obj in removed:
            obj.unlink()
        print(f"  removed {len(removed)} object file(s) for {m.target}")

        output = rebuild(m.target)
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {m.target}: {compiled} 'Building CXX' line(s)")

        code, test_output = run_test(m.target, m.test)
        killed = code != 0
        print(f"  {m.target} --gtest_filter={m.test} -> exit {code} "
              f"({'RED, mutation killed' if killed else 'GREEN, MUTATION SURVIVED'})")
        if not killed:
            print("  ----- surviving mutation, test output -----")
            print("\n".join(test_output.splitlines()[-25:]))
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit("restore failed: the file does not hash back to its original")
        for obj in object_files(m.target):
            obj.unlink()

    return killed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<28} {m.target:<30} {m.test}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD} is not configured; run\n  cmake --preset venue-lite")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = sorted({m.target for m in selected})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    width = max(len(m.name) for m, _ in results)
    for m, killed in results:
        print(f"  {m.name:<{width}}  {m.target:<30}  "
              f"{'RED (killed)' if killed else 'GREEN (SURVIVED)'}")
    survivors = [m.name for m, killed in results if not killed]
    print()
    if survivors:
        print(f"{len(survivors)} mutation(s) survived: {', '.join(survivors)}")
    if not restored:
        print("the control run after the mutations is not green: the tree did not come back")
    if survivors or not restored:
        return 1
    print(f"all {len(results)} mutations killed; control green before and after")
    return 0


if __name__ == "__main__":
    sys.exit(main())
