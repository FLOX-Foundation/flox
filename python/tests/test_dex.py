"""
python/tests/test_dex.py -- the flox_py.dex comfort layer (W24-T002).

Symbol- and decimals-aware pricing over the exact curves. Verifies every quote still
matches the raw flox_py curve to the wei, that human construction works, that the
silent-wrong-answer footgun now raises, and that CLMM state reads back.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_dex.py
"""
import os
import sys
from decimal import Decimal

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox  # noqa: E402
from flox_py import dex  # noqa: E402

WETH, USDC = dex.Token("WETH", 18), dex.Token("USDC", 6)


def test_amount_and_token():
    a = dex.Amount.parse("50_000 USDC", {"USDC": USDC, "WETH": WETH})
    assert a.wei == 50_000 * 10**6 and a.token == USDC
    assert WETH.to_wei("1.5") == 15 * 10**17
    assert repr(WETH.amount("2.5")) == "2.5 WETH"


def test_constant_product_human_and_exact():
    p = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    assert p.spot_price == Decimal("2000")          # exact mid, fee-excluded
    q = p.quote("10 WETH")
    # Matches the raw curve to the wei.
    assert q.exact_wei == flox.AmmCurve.constant_product(1000*10**18, 2_000_000*10**6, 997, 1000
                                                         ).amount_out(0, 1, 10*10**18)
    assert q.out.token == USDC and q.price_impact > 0
    # PancakeSwap-style 0.25% maps to its own fee.
    pc = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.25%", fee_den=10000)
    assert pc.quote("10 WETH").exact_wei == flox.AmmCurve.constant_product(
        1000*10**18, 2_000_000*10**6, 9975, 10000).amount_out(0, 1, 10*10**18)


def test_uniswap_v3_readback_and_from_price():
    # token0=USDC(6), token1=WETH(18) -- the live 0.05% snapshot.
    v3 = dex.UniswapV3(USDC, WETH, 1959100328691929984878240664321702, 2580696918646962643, "0.05%")
    assert v3.quote("1000 USDC").exact_wei == 611128907033491490     # QuoterV2, to the wei
    assert v3.sqrt_price == 1959100328691929984878240664321702        # read-back (W24-T001)
    assert v3.liquidity == 2580696918646962643
    # from_price reconstructs a sqrtPriceX96 close to the real one (same human price).
    built = dex.UniswapV3.from_price(USDC, WETH, price=v3.spot_price, liquidity=2580696918646962643,
                                     fee="0.05%")
    assert abs(built.sqrt_price - v3.sqrt_price) / v3.sqrt_price < Decimal("1e-15")


def test_raydium_cp_fee():
    ray = dex.RaydiumCp(dex.Token("WSOL", 9), USDC, reserves=("13831.187668587 WSOL",
                        "13771991.024747 USDC"), trade_fee="0.25%")
    assert ray.quote("1 WSOL").exact_wei == flox.AmmCurve.raydium_cp(
        13831187668587, 13771991024747, 2500, 0, True).amount_out(0, 1, 10**9)


def test_guardrail_kills_the_footgun():
    # The W1-suite bug: token0=USDC here; quoting a WETH amount the wrong way must raise,
    # not return a silent ~0.
    v3 = dex.UniswapV3(USDC, WETH, 1959100328691929984878240664321702, 2580696918646962643, "0.05%")
    # An unknown token raises.
    try:
        v3.quote("1 DAI"); raise AssertionError("expected unknown-token error")
    except ValueError:
        pass
    # A dust WETH amount that would vanish to 0 raises instead of lying.
    raised = False
    try:
        v3.quote("0.000000000001 WETH")
    except ValueError:
        raised = True
    assert raised, "a vanishing output must raise, not return a quiet 0"


def test_clone_and_depth():
    p = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    c = p.clone()
    p.swap("100 WETH")
    assert c.reserves[0].wei == 1000 * 10**18      # clone untouched
    assert p.reserves[0].wei == 1100 * 10**18      # original moved
    rows = c.depth(["1 WETH", "50 WETH", "200 WETH"])
    impacts = [r["impact_pct"] for r in (rows if isinstance(rows, list)
                                         else rows.to_dict("records"))]
    assert impacts == sorted(impacts) and impacts[0] > 0   # monotone slippage


def test_cross_venue_and_arb_one_liners():
    # Best execution: same notional, the two CP venues (v3 is a different pool scale).
    v2 = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    ray = dex.RaydiumCp(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), trade_fee="0.25%")
    outs = {p.venue: p.quote("50_000 USDC").out.human for p in (v2, ray)}
    assert outs["RaydiumCp"] > outs["UniswapV2"]   # lower fee -> more out


if __name__ == "__main__":
    for fn in (test_amount_and_token, test_constant_product_human_and_exact,
               test_uniswap_v3_readback_and_from_price, test_raydium_cp_fee,
               test_guardrail_kills_the_footgun, test_clone_and_depth,
               test_cross_venue_and_arb_one_liners):
        fn()
        print("ok:", fn.__name__)
    print("test_dex: OK")
