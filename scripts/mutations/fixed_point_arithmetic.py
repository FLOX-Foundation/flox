#!/usr/bin/env python3
"""Mutation harness for the fixed-point arithmetic fixes.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each fix one at a
time, in the source, and checks that the test written for it goes red -- and
that an unmutated tree goes green before and after.

Every run is honest about the build: the mutated file's hash is printed before
and after, the target's object files are deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" or the run is refused,
and the test binary runs under a timeout.

Usage:

    python3 scripts/mutations/fixed_point_arithmetic.py            # control, all mutations, control
    python3 scripts/mutations/fixed_point_arithmetic.py --list
    python3 scripts/mutations/fixed_point_arithmetic.py --only decimal-mul-overflowing-split

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON
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
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 120

DECIMAL = "include/flox/util/base/decimal.h"
SCALE_CHECK = "include/flox/util/base/scale_check.h"
BOOK = "include/flox/book/nlevel_order_book.h"

NATIVE = "test_decimal_portable"
FORCED = "test_decimal_portable_forced"
PLAIN = "test_decimal"
MULDIV = "test_fixed_point_muldiv"


@dataclass
class Mutation:
    name: str
    why: str
    file: str
    old: str
    new: str
    target: str
    test: str
    occurrence: int = 1
    expected_occurrences: int = 1


MUTATIONS: list[Mutation] = [
    Mutation(
        name="decimal-mul-overflowing-split",
        why="restores the hand-rolled quotient+remainder split; its remainder term is 7.4e19 at an ordinary BTC price",
        file=DECIMAL,
        old="return Decimal(mulDivI64As<Portable>(_raw, other._raw, Scale));",
        new="return Decimal((_raw / Scale) * other._raw + (_raw % Scale) * other._raw / Scale);",
        target=NATIVE,
        test="DecimalPaths.MultiplyAgreesOnThePriceThatUsedToOverflow",
    ),
    Mutation(
        name="decimal-mul-overflowing-split-forced",
        why="the same break, seen through the operator rather than the explicit path, in the forced-portable build",
        file=DECIMAL,
        old="return Decimal(mulDivI64As<Portable>(_raw, other._raw, Scale));",
        new="return Decimal((_raw / Scale) * other._raw + (_raw % Scale) * other._raw / Scale);",
        target=FORCED,
        test="DecimalPortable.MultiplyIsExactAtAnOrdinaryPrice",
    ),
    Mutation(
        name="decimal-div-overflowing-numerator",
        why="restores `raw * Scale` as the numerator; 6e12 * 1e8 leaves the int64 range at one BTC price",
        file=DECIMAL,
        old="return Decimal(mulDivI64As<Portable>(_raw, Scale, other._raw));",
        new="return Decimal((_raw * Scale) / other._raw);",
        target=NATIVE,
        test="DecimalPortable.DivideIsExactAtAnOrdinaryPrice",
    ),
    Mutation(
        name="decimal-rescale-overflowing-product",
        why="restores `_raw * toScale` before the division; a 1e15-scale raw makes that 1e25",
        file=DECIMAL,
        old="return withScale(mulDivI64As<Portable>(_raw, toScale, fromScale), toScale);",
        new="return withScale(static_cast<int64_t>(_raw * toScale / fromScale), toScale);",
        target=FORCED,
        test="DecimalPortable.RescaleIsExactFromADexScale",
    ),
    Mutation(
        name="decimal-minus-equals-unchecked",
        why="drops the overflow check from operator-=, the finding's own example",
        file=DECIMAL,
        old="_raw = checkedSubI64(_raw, other._raw);",
        new="_raw -= other._raw;",
        target=NATIVE,
        test="DecimalOverflow.MinusEqualsSaturatesInsteadOfWrapping",
    ),
    Mutation(
        name="decimal-binary-plus-unchecked",
        why="drops the overflow check from the binary operator+",
        file=DECIMAL,
        old="return withScale(checkedAddI64(_raw, d._raw), _raw != 0 ? scale() : d.scale());",
        new="return withScale(_raw + d._raw, _raw != 0 ? scale() : d.scale());",
        target=NATIVE,
        test="DecimalOverflow.PlusEqualsAndBinaryPlusAgree",
    ),
    Mutation(
        name="decimal-binary-minus-unchecked",
        why="drops the overflow check from the binary operator-",
        file=DECIMAL,
        old="return withScale(checkedSubI64(_raw, d._raw), _raw != 0 ? scale() : d.scale());",
        new="return withScale(_raw - d._raw, _raw != 0 ? scale() : d.scale());",
        target=NATIVE,
        test="DecimalOverflow.BinaryMinusSaturatesInsteadOfWrapping",
    ),
    Mutation(
        name="decimal-scalar-multiply-unchecked",
        why="drops the overflow check from Decimal * int64_t",
        file=DECIMAL,
        old="return withScale(checkedMulI64(_raw, x), scale());",
        new="return withScale(_raw * x, scale());",
        target=NATIVE,
        test="DecimalOverflow.ScalarMultiplySaturatesOnBothSides",
    ),
    Mutation(
        name="decimal-scalar-multiply-friend-unchecked",
        why="drops the overflow check from int64_t * Decimal, the operand order the member does not cover",
        file=DECIMAL,
        old="return withScale(checkedMulI64(x, d._raw), d.scale());",
        new="return withScale(x * d._raw, d.scale());",
        target=NATIVE,
        test="DecimalOverflow.ScalarMultiplySaturatesOnBothSides",
    ),
    Mutation(
        name="decimal-scalar-divide-minus-one-unguarded",
        why="removes the INT64_MIN / -1 guard, the one division that overflows",
        file=DECIMAL,
        old="""    if (x == -1)
    {
      return withScale(checkedSubI64(0, _raw), scale());
    }
""",
        new="",
        target=NATIVE,
        test="DecimalOverflow.ScalarDivideByMinusOneSaturatesAtTheBoundary",
    ),
    Mutation(
        name="orderbook-ask-notional-unchecked-narrowing",
        why="restores the bare static_cast on consumeAsks' 128-bit notional",
        file=BOOK,
        old="Volume::fromRaw(checkedNarrowI64(notionalRaw2 / Volume::Scale))};",
        new="Volume::fromRaw(static_cast<int64_t>(notionalRaw2 / Volume::Scale))};",
        target=NATIVE,
        test="OrderBookNotional.ADeepAskConsumeSaturatesInsteadOfGoingNegative",
        occurrence=1,
        expected_occurrences=2,
    ),
    Mutation(
        name="orderbook-bid-notional-unchecked-narrowing",
        why="restores the bare static_cast on consumeBids' 128-bit notional, the second site",
        file=BOOK,
        old="Volume::fromRaw(checkedNarrowI64(notionalRaw2 / Volume::Scale))};",
        new="Volume::fromRaw(static_cast<int64_t>(notionalRaw2 / Volume::Scale))};",
        target=NATIVE,
        test="OrderBookNotional.ADeepBidConsumeSaturatesInsteadOfGoingNegative",
        occurrence=2,
        expected_occurrences=2,
    ),
    # --- Added after a mull run over decimal.h / scale_check.h. -------------
    #
    # The mutations above were written by hand from the fixes they guard, and
    # they all mutate the same thing: whether the overflow check still runs.
    # An automatic run over the same two headers found a second bug class on
    # the very same lines -- the ordinary arithmetic under the check, and the
    # comparisons at their exact thresholds -- which nobody thinks to break by
    # hand precisely because it looks too simple to get wrong.
    #
    # Everything below is red in the build this script documents
    # (RelWithDebInfo, so NDEBUG, so FLOX_SCALE_CHECKS off). The scale
    # bookkeeping that operator+ / operator- / operator+= / operator-= do on a
    # zero operand is NOT here: the runtime scale only exists with
    # FLOX_SCALE_CHECKS on, so those mutations are equivalent in this build and
    # only go red under -DCMAKE_BUILD_TYPE=Debug. Their tests live in
    # tests/test_decimal.cpp behind #if FLOX_SCALE_CHECKS.
    Mutation(
        name="decimal-scalar-divide-multiplies-instead",
        why="swaps the / for a * in the tail of Decimal / int64_t, past both of its guards",
        file=DECIMAL,
        old="return withScale(_raw / x, scale());",
        new="return withScale(_raw * x, scale());",
        target=NATIVE,
        test="DecimalOverflow.ScalarDivideIsExactOnEverySignCombination",
    ),
    Mutation(
        name="decimal-less-than-includes-equal",
        why="widens operator< to <=, which only shows on two equal values",
        file=DECIMAL,
        old="constexpr bool operator<(const Decimal& other) const { return _raw < other._raw; }",
        new="constexpr bool operator<(const Decimal& other) const { return _raw <= other._raw; }",
        target=PLAIN,
        test="DecimalTest.ComparisonOperatorsAtTheExactThreshold",
    ),
    Mutation(
        name="decimal-greater-than-includes-equal",
        why="widens operator> to >=, the same blind spot on the other side",
        file=DECIMAL,
        old="constexpr bool operator>(const Decimal& other) const { return _raw > other._raw; }",
        new="constexpr bool operator>(const Decimal& other) const { return _raw >= other._raw; }",
        target=PLAIN,
        test="DecimalTest.ComparisonOperatorsAtTheExactThreshold",
    ),
    Mutation(
        name="decimal-less-equal-excludes-equal",
        why="narrows operator<= to <, so an equal price stops counting as within a limit",
        file=DECIMAL,
        old="constexpr bool operator<=(const Decimal& other) const { return _raw <= other._raw; }",
        new="constexpr bool operator<=(const Decimal& other) const { return _raw < other._raw; }",
        target=PLAIN,
        test="DecimalTest.ComparisonOperatorsAtTheExactThreshold",
    ),
    Mutation(
        name="decimal-greater-equal-excludes-equal",
        why="narrows operator>= to >, the same on the other side",
        file=DECIMAL,
        old="constexpr bool operator>=(const Decimal& other) const { return _raw >= other._raw; }",
        new="constexpr bool operator>=(const Decimal& other) const { return _raw > other._raw; }",
        target=PLAIN,
        test="DecimalTest.ComparisonOperatorsAtTheExactThreshold",
    ),
    Mutation(
        name="scale-check-negate-low-word-subtracts",
        why="breaks the two's-complement negation in checkedMulI64, so every negative scalar product that fits is off by two",
        file=SCALE_CHECK,
        old="return static_cast<int64_t>(~lo + 1u);",
        new="return static_cast<int64_t>(~lo - 1u);",
        target=NATIVE,
        test="DecimalOverflow.ScalarMultiplyIsExactAtTheLastValueBeforeTheLimit",
    ),
    Mutation(
        name="scale-check-subtract-saturation-sign-excludes-zero",
        why="makes an overflowing subtraction from a zero minuend saturate downward instead of up",
        file=SCALE_CHECK,
        old="return a >= 0 ? (std::numeric_limits<int64_t>::max)() : (std::numeric_limits<int64_t>::min)();",
        new="return a > 0 ? (std::numeric_limits<int64_t>::max)() : (std::numeric_limits<int64_t>::min)();",
        target=NATIVE,
        test="DecimalOverflow.BinaryMinusSaturatesInsteadOfWrapping",
    ),
    Mutation(
        name="muldiv-high-word-guard-off-by-one",
        why="lets a quotient of exactly 2^64 past the guard, where the shift-subtract answers 0 instead of saturating",
        file=SCALE_CHECK,
        old="  if (hi >= ud)",
        new="  if (hi > ud)",
        target=MULDIV,
        test="FixedPointMulDiv.TheHighWordGuardTripsWhenItReachesTheDivisor",
    ),
    Mutation(
        name="muldiv-zero-divisor-sign-native",
        why="inverts the zero test that decides whether a division by zero saturates or returns zero",
        file=SCALE_CHECK,
        old="  const bool nonZeroProduct = (a != 0) && (b != 0);",
        new="  const bool nonZeroProduct = (a == 0) && (b != 0);",
        target=MULDIV,
        test="FixedPointMulDiv.AZeroDivisorSaturatesWithTheProductsOwnSign",
        occurrence=1,
        expected_occurrences=2,
    ),
    Mutation(
        name="muldiv-zero-divisor-sign-portable",
        why="the same break in mulDivI64Portable, the copy no toolchain here compiles by default",
        file=SCALE_CHECK,
        old="  const bool nonZeroProduct = (a != 0) && (b != 0);",
        new="  const bool nonZeroProduct = (a != 0) && (b == 0);",
        target=MULDIV,
        test="FixedPointMulDiv.AZeroDivisorSaturatesWithTheProductsOwnSign",
        occurrence=2,
        expected_occurrences=2,
    ),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_occurrence(text: str, old: str, new: str, occurrence: int, expected: int) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(
            f"mutation anchor found {count} time(s), expected {expected}:\n  {old!r}"
        )
    start = -1
    for _ in range(occurrence):
        start = text.index(old, start + 1)
    return text[:start] + new + text[start + len(old):]


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
    binary = BUILD / "tests" / target
    cmd = [str(binary)]
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
        summary = next((line for line in output.splitlines() if line.startswith("[==========] ")
                        and " ran." in line), "")
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

    mutated = replace_occurrence(original, m.old, m.new, m.occurrence, m.expected_occurrences)
    if mutated == original:
        raise SystemExit("mutation changed nothing")
    path.write_text(mutated)
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
            print(f"{m.name:<44} {m.target:<30} {m.test}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON"
        )

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
    for m, killed in results:
        print(f"  {'RED  ' if killed else 'ALIVE'}  {m.name:<44} {m.test}")
    survived = [m.name for m, killed in results if not killed]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
