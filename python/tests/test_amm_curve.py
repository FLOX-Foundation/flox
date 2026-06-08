"""
python/tests/test_amm_curve.py — price a DEX swap from Python (W1-T040).

The exact AMM curves, bound as AmmCurve, price a swap to the wei with int amounts.
Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_amm_curve.py
"""

import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox  # noqa: E402


def main():
    # Constant product (Uniswap v2, 997/1000), reserves 1000 / 2000 (18 decimals).
    cp = flox.AmmCurve.constant_product(1000 * 10**18, 2000 * 10**18, 997, 1000)
    assert cp.token_count() == 2
    assert cp.amount_out(0, 1, 10**18) == 1992013962079806432, "constant-product to the wei"
    assert cp.balances()[0] == 1000 * 10**18

    # apply_swap moves the pool; a clone is independent.
    clone = cp.clone()
    cp.apply_swap(0, 1, 10**18)
    assert cp.balances()[0] == 1001 * 10**18
    assert clone.balances()[0] == 1000 * 10**18, "clone is independent"

    # Uniswap v3, the live USDC/WETH 0.05% snapshot, in range: 1000 USDC -> WETH.
    v3 = flox.AmmCurve.uniswap_v3(1959100328691929984878240664321702, 2580696918646962643, 500)
    assert v3.amount_out(0, 1, 1000000000) == 611128907033491490, "v3 matches QuoterV2"

    # Raydium CP (Solana), 0.25% trade fee, no creator fee.
    ray = flox.AmmCurve.raydium_cp(13831187668587, 13771991024747, 2500)
    assert ray.amount_out(0, 1, 1000000000) > 0

    # CLMM state read-back: v3 returns its exact sqrt price + liquidity, others None.
    assert v3.sqrt_price() == 1959100328691929984878240664321702, "v3 sqrt price read-back"
    assert v3.liquidity() == 2580696918646962643, "v3 liquidity read-back"
    assert cp.sqrt_price() is None and ray.sqrt_price() is None, "non-CLMM returns None"

    print("test_amm_curve: OK (constant-product + v3 to the wei, clone, raydium, CLMM read-back)")


if __name__ == "__main__":
    main()
