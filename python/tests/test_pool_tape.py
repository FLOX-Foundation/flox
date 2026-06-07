"""
python/tests/test_pool_tape.py — replay a recorded DEX pool history (W1-T041).

A pool-state tape is a delta log; the pool state is derived by replaying the deltas
through the exact curve. Build a tape, replay it, and the reconstruction matches the
C++ replay with zero drift. Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_pool_tape.py
"""

import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox  # noqa: E402


def main():
    r0, r1 = 1000 * 10**18, 2000 * 10**18

    # Final reserves after the two swaps, from the exact curve (what the closing
    # checkpoint must match).
    ref = flox.AmmCurve.constant_product(r0, r1, 997, 1000)
    ref.apply_swap(0, 1, 5 * 10**18)
    ref.apply_swap(1, 0, 3 * 10**18)
    f0, f1 = ref.balances()[0], ref.balances()[1]

    tape = flox.PoolTape()
    tape.descriptor_constant_product(997, 1000, 18, 18)
    tape.checkpoint(100, r0, r1)
    tape.swap(200, True, 5 * 10**18)   # base in
    tape.swap(300, False, 3 * 10**18)  # quote in
    tape.checkpoint(400, f0, f1)       # matches -> no drift

    replay = tape.replay(0, 1, 18, 18)
    assert replay.drift_count() == 0, "no drift on a matching tape"
    assert replay.trade_count() == 2
    curve = replay.curve()
    assert curve.balances()[0] == f0 and curve.balances()[1] == f1, "reconstructed to the wei"

    # A mismatched closing checkpoint is caught as drift.
    bad = flox.PoolTape()
    bad.descriptor_constant_product(997, 1000, 18, 18)
    bad.checkpoint(100, r0, r1)
    bad.swap(200, True, 5 * 10**18)
    bad.checkpoint(300, r0, r1)  # unchanged -> disagrees with the swap
    assert bad.replay().drift_count() == 1, "mismatched checkpoint is drift"

    print("test_pool_tape: OK (replay reconstructs to the wei, drift detection)")


if __name__ == "__main__":
    main()
