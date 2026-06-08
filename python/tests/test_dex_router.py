"""
python/tests/test_dex_router.py -- router / arb / chain ingest (W24-T004).

Best-execution routing, depth-aware cross-pool arb sizing, and the address-to-Pool /
address-to-Tape ingest adapters. The arb is checked against an actual clone-based
execution to the wei; the ingest adapters run against recorded fixtures through an
injected fake RPC, so CI never touches the network.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_dex_router.py
"""
import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

from flox_py import dex  # noqa: E402

WETH, USDC = dex.Token("WETH", 18), dex.Token("USDC", 6)


def _word(n: int) -> str:
    return (n).to_bytes(32, "big").hex()


def test_router_best_matches_direct_quote():
    uni = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    ray = dex.RaydiumCp(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), trade_fee="0.25%")
    r = dex.Router([uni, ray])
    pool, q = r.best("50_000 USDC", into="WETH")
    # Lower fee wins; the chosen quote is exactly that pool's own quote.
    assert pool is ray
    assert q.out.wei == ray.quote("50_000 USDC", into="WETH").out.wei
    # The comparison table is sorted best-first.
    rows = r.table("50_000 USDC", into="WETH")
    recs = rows if isinstance(rows, list) else rows.to_dict("records")
    assert recs[0]["venue"] == "RaydiumCp"
    assert recs[0]["out_human"] >= recs[1]["out_human"]


def test_arb_interior_optimum_matches_clone_execution():
    # Two pools, same pair, different prices -> a profitable cross-pool arb.
    cheap = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    dear = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_100_000 USDC"), fee="0.30%")
    a = dex.arb(cheap, dear)
    assert a.profitable and a.profit.wei > 0
    assert 0 < a.size.wei < dear.reserves[1].wei            # an interior optimum
    assert a.route == ("UniswapV2", "UniswapV2")            # buy WETH on cheap, sell on dear

    # Re-run the route on clones and confirm the realised profit is exactly a.profit.
    buy, sell = cheap.clone(), dear.clone()                 # cheap WETH is on `cheap`
    t0_out = buy.swap(USDC.amount(USDC.to_human(a.size.wei)), into="WETH").out
    t1_back = sell.swap(WETH.amount(t0_out.human), into="USDC").out
    assert t1_back.wei - a.size.wei == a.profit.wei

    # No edge when the pools are identical.
    same = dex.arb(cheap.clone(), cheap.clone())
    assert not same.profitable and same.route is None


def test_arb_profit_is_the_maximum():
    cheap = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    dear = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_100_000 USDC"), fee="0.30%")
    a = dex.arb(cheap, dear)

    def profit_at(x):
        t0 = cheap._c.amount_out(1, 0, x)            # USDC(1)->WETH(0) on cheap
        return dear._c.amount_out(0, 1, t0) - x      # WETH->USDC on dear

    # No nearby size beats the reported optimum.
    for dx in (-1000, -10, -1, 1, 10, 1000):
        assert profit_at(a.size.wei + dx) <= a.profit.wei


def _fake_rpc(table):
    """An RPC double: table maps (method) -> result, or (method, key) for eth_call data."""
    def rpc(method, params):
        if method == "eth_call":
            return table[("eth_call", params[0]["data"])]
        return table[method]
    return rpc


def test_from_evm_pool_v2_and_v3():
    # v2 getReserves(): (reserve0, reserve1, blockTimestampLast).
    v2_reserves = "0x" + _word(1000 * 10**18) + _word(2_000_000 * 10**6) + _word(0)
    rpc_v2 = _fake_rpc({("eth_call", "0x0902f1ac"): v2_reserves})
    pool = dex.from_evm_pool("0xpool", WETH, USDC, rpc_v2, kind="v2", fee="0.30%")
    assert pool.reserves[0].wei == 1000 * 10**18
    assert pool.reserves[1].wei == 2_000_000 * 10**6

    # v3 slot0() first word is sqrtPriceX96; liquidity() is one word.
    slot0 = "0x" + _word(1959100328691929984878240664321702) + _word(0) * 6
    liq = "0x" + _word(2580696918646962643)
    rpc_v3 = _fake_rpc({("eth_call", "0x3850c7bd"): slot0,
                        ("eth_call", "0x1a686502"): liq})
    v3 = dex.from_evm_pool("0xpool", USDC, WETH, rpc_v3, kind="v3", fee="0.05%")
    assert v3.sqrt_price == 1959100328691929984878240664321702
    assert v3.quote("1000 USDC").exact_wei == 611128907033491490   # the known snapshot


def test_tape_from_evm_logs():
    pool = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    one_weth = _word(10**18)
    zero = _word(0)
    logs = [{"blockNumber": "0x10", "data": "0x" + one_weth + zero + zero + zero}]
    rpc = _fake_rpc({"eth_getLogs": logs})
    tape = dex.tape_from_evm_logs(pool, rpc, from_block=15, to_block=17, address="0xpool")
    bt = tape.replay()
    rows = bt if isinstance(bt, list) else bt.to_dict("records")
    c = dex.UniswapV2(WETH, USDC, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    c.swap("1 WETH")
    assert rows[-1]["reserve0"] == c.reserves[0].wei


def test_tape_from_solana_vault_deltas():
    WSOL = dex.Token("WSOL", 9)
    pool = dex.RaydiumCp(WSOL, USDC, reserves=("13831.187668587 WSOL",
                         "13771991.024747 USDC"), trade_fee="0.25%")
    # A swap that sold 1 WSOL into vault0 and paid USDC out of vault1.
    tx = {
        "blockTime": 1700,
        "transaction": {"message": {"accountKeys": [{"pubkey": "vault0"},
                                                    {"pubkey": "vault1"}]}},
        "meta": {
            "preTokenBalances": [{"accountIndex": 0, "uiTokenAmount": {"amount": "0"}},
                                 {"accountIndex": 1, "uiTokenAmount": {"amount": "1000000000"}}],
            "postTokenBalances": [{"accountIndex": 0, "uiTokenAmount": {"amount": "1000000000"}},
                                  {"accountIndex": 1, "uiTokenAmount": {"amount": "998000000"}}],
        },
    }
    rpc = _fake_rpc({"getTransaction": tx})
    tape = dex.tape_from_solana(pool, ["sig1"], rpc, vault0="vault0", vault1="vault1")
    bt = tape.replay()
    rows = bt if isinstance(bt, list) else bt.to_dict("records")
    # vault0 (WSOL) gained 1 WSOL -> a token0-for-token1 sell of 1e9 wei.
    c = dex.RaydiumCp(WSOL, USDC, reserves=("13831.187668587 WSOL",
                      "13771991.024747 USDC"), trade_fee="0.25%")
    c.swap("1 WSOL")
    assert rows[-1]["reserve0"] == c.reserves[0].wei
    assert rows[0]["ts"] == 1700


if __name__ == "__main__":
    for fn in (test_router_best_matches_direct_quote,
               test_arb_interior_optimum_matches_clone_execution,
               test_arb_profit_is_the_maximum,
               test_from_evm_pool_v2_and_v3,
               test_tape_from_evm_logs,
               test_tape_from_solana_vault_deltas):
        fn()
        print("ok:", fn.__name__)
    print("test_dex_router: OK")
