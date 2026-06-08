"""
python/tests/test_dex_tape.py -- the flox_py.dex tape/backtest layer (W24-T003).

A pool-state tape replays swaps through the exact curve into a table of price /
reserves / LP value / impermanent loss / drift. These tests pin the replay to the raw
flox_py curve to the wei, check the IL sign and magnitude against the constant-product
closed form, and check that a planted checkpoint mismatch is flagged as drift.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_dex_tape.py
"""
import os
import sys
from decimal import Decimal

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox  # noqa: E402
from flox_py import dex  # noqa: E402

WETH, USDC = dex.Token("WETH", 18), dex.Token("USDC", 6)


def _rows(bt):
    return bt if isinstance(bt, list) else bt.to_dict("records")


def test_replay_matches_raw_curve_to_the_wei():
    pool = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    tape = dex.Tape(pool).from_swaps([(1, "50 WETH"), (2, "50 WETH"), (3, "100000 USDC")])
    bt = tape.replay()
    rows = _rows(bt)

    # Drive the raw curve by hand and compare reserves step by step.
    c = flox.AmmCurve.constant_product(1000 * 10**18, 2_000_000 * 10**6, 997, 1000)
    c.apply_swap(0, 1, 50 * 10**18)
    c.apply_swap(0, 1, 50 * 10**18)
    c.apply_swap(1, 0, 100_000 * 10**6)
    bal = c.balances()
    assert rows[-1]["reserve0"] == bal[0]
    assert rows[-1]["reserve1"] == bal[1]
    # The source pool is untouched -- replay works on a clone.
    assert pool.reserves[0].wei == 1000 * 10**18


def test_il_is_zero_at_entry_and_negative_after_a_move():
    pool = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    pos = dex.LpPosition(pool, value="100000 USDC")
    assert pos.impermanent_loss() == 0                  # no move yet
    pool.swap("200 WETH")                               # price drops
    il = pos.impermanent_loss()
    assert il < 0                                       # LP underperforms HODL after a move
    assert pos.value() < pos.hodl_value()


def test_il_matches_constant_product_closed_form():
    # Fee-free CP pool so the closed form IL = 2*sqrt(r)/(1+r) - 1 holds exactly,
    # where r = price_now / price_entry.
    pool = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"),
                         fee="0%", fee_den=1)
    pos = dex.LpPosition(pool)
    p0 = pool.spot_price
    pool.swap("100 WETH")
    p1 = pool.spot_price
    r = p1 / p0
    closed = 2 * Decimal(r).sqrt() / (1 + r) - 1
    got = pos.impermanent_loss()
    assert abs(got - closed) < Decimal("1e-9"), (got, closed)


def test_checkpoint_drift_is_flagged():
    pool = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    tape = dex.Tape(pool)
    tape.swap(1, "50 WETH")
    # A checkpoint that disagrees with the replayed reserves -> drift.
    tape.checkpoint(2, "999 WETH", "2_000_000 USDC")
    bt = tape.replay()
    rows = _rows(bt)
    drift_rows = [row for row in rows if row["drift"]]
    assert len(drift_rows) == 1
    assert (bt.attrs["drift_count"] if not isinstance(bt, list) else
            sum(r["drift"] for r in rows)) == 1


def test_from_evm_logs_decodes_v2_swaps():
    pool = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    # amount0In = 1 WETH, the rest zero (a token0-for-token1 sell).
    one_weth = (10**18).to_bytes(32, "big").hex()
    zero = (0).to_bytes(32, "big").hex()
    log = {"blockNumber": "0x10", "data": "0x" + one_weth + zero + zero + zero}
    tape = dex.Tape.from_evm_logs(pool, [log])
    bt = tape.replay()
    rows = _rows(bt)
    c = flox.AmmCurve.constant_product(1000 * 10**18, 2_000_000 * 10**6, 997, 1000)
    c.apply_swap(0, 1, 10**18)
    assert rows[-1]["reserve0"] == c.balances()[0]
    assert rows[0]["ts"] == 16


if __name__ == "__main__":
    for fn in (test_replay_matches_raw_curve_to_the_wei,
               test_il_is_zero_at_entry_and_negative_after_a_move,
               test_il_matches_constant_product_closed_form,
               test_checkpoint_drift_is_flagged,
               test_from_evm_logs_decodes_v2_swaps):
        fn()
        print("ok:", fn.__name__)
    print("test_dex_tape: OK")
