"""
python/tests/test_dex_amount.py — the 256-bit DEX amount boundary (W1-T039).

A u256 / i256 crosses the binding as a native Python int, so a 256-bit wei amount is
lossless. Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_dex_amount.py
"""

import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox  # noqa: E402

U256_MAX = 2**256 - 1


def main():
    # A value up to 2^256 - 1 survives the exact u256 to the wei.
    assert flox.u256_roundtrip(U256_MAX) == U256_MAX, "u256 max"
    assert flox.u256_roundtrip(0) == 0
    assert flox.u256_roundtrip(2**128 + 7) == 2**128 + 7

    # i256 carries the sign.
    assert flox.i256_roundtrip(-(10**30)) == -(10**30), "i256 negative"
    assert flox.i256_roundtrip(42) == 42
    assert flox.i256_roundtrip(0) == 0

    # Hex (with or without 0x).
    assert flox.u256_from_hex("0xff") == 255
    assert flox.u256_from_hex("100") == 256
    assert flox.u256_from_hex(hex(U256_MAX)) == U256_MAX

    # Out-of-range / bad input raises rather than truncating.
    for bad in ("not a number",):
        try:
            flox.u256_from_hex(bad)
        except Exception:
            pass
        else:
            raise AssertionError("expected u256_from_hex to reject bad input")

    print("test_dex_amount: OK (u256 max, i256 sign, hex round-trip)")


if __name__ == "__main__":
    main()
