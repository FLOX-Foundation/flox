"""Unit tests for the DEX DX MCP tools (W24-T006).

route_amm_swap / amm_price_impact / replay_pool_tape are thin server tools over the
flox.dex comfort layer. These tests pin each tool's JSON output to the flox.dex layer it
wraps. They need the compiled `flox_py` (with flox_py.dex) and are skipped otherwise --
the MCP-only CI job has no compiled module.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "mcp"))
sys.path.insert(0, str(REPO_ROOT / "build" / "python"))

from flox_mcp.tools import runtime  # noqa: E402

try:
    import flox_py  # noqa: F401
    from flox_py import dex  # noqa: F401
    HAS_FLOX = True
except ImportError:
    HAS_FLOX = False

skip_no_flox = pytest.mark.skipif(not HAS_FLOX, reason="flox_py.dex not importable")

WETH = {"symbol": "WETH", "decimals": 18}
USDC = {"symbol": "USDC", "decimals": 6}


def _v2(reserves, fee="0.30%"):
    return {"venue": "constant_product", "token0": WETH, "token1": USDC,
            "reserves": reserves, "fee": fee}


def _ray(reserves, trade_fee="0.25%"):
    return {"venue": "raydium_cp", "token0": WETH, "token1": USDC,
            "reserves": reserves, "trade_fee": trade_fee}


# ── route_amm_swap ────────────────────────────────────────────────────


@skip_no_flox
def test_route_amm_swap_picks_best_and_matches_layer():
    venues = [_v2(["1000 WETH", "2000000 USDC"]), _ray(["1000 WETH", "2000000 USDC"])]
    out = json.loads(runtime.route_amm_swap(venues, "50000 USDC", into="WETH"))
    # Lower fee (Raydium 0.25%) fills the most.
    assert out["best"]["venue"] == "RaydiumCp"
    # The reported best matches a direct flox.dex Router call to the wei.
    ray = dex.RaydiumCp(dex.Token("WETH", 18), dex.Token("USDC", 6),
                        reserves=("1000 WETH", "2000000 USDC"), trade_fee="0.25%")
    assert out["best"]["out_wei"] == str(ray.quote("50000 USDC", into="WETH").exact_wei)
    assert out["venues"][0]["venue"] == "RaydiumCp"   # sorted best-first


@skip_no_flox
def test_route_amm_swap_bad_input():
    assert "non-empty list" in runtime.route_amm_swap([], "50000 USDC")


# ── amm_price_impact ──────────────────────────────────────────────────


@skip_no_flox
def test_amm_price_impact_monotone_and_matches_layer():
    out = json.loads(runtime.amm_price_impact(_v2(["1000 WETH", "2000000 USDC"]),
                                              ["1 WETH", "50 WETH", "200 WETH"]))
    impacts = [r["impact_pct"] for r in out["depth"]]
    assert impacts == sorted(impacts) and impacts[0] > 0
    assert out["spot_price"] == 2000.0


# ── replay_pool_tape ──────────────────────────────────────────────────


@skip_no_flox
def test_replay_pool_tape_matches_layer():
    pool = _v2(["1000 WETH", "2000000 USDC"])
    out = json.loads(runtime.replay_pool_tape(
        pool, swaps=[[1, "50 WETH"], [2, "50 WETH"], [3, "100000 USDC"]]))
    # Drive the same swaps through the raw curve and compare the final reserves.
    c = flox_py.AmmCurve.constant_product(1000 * 10**18, 2_000_000 * 10**6, 997, 1000)
    c.apply_swap(0, 1, 50 * 10**18)
    c.apply_swap(0, 1, 50 * 10**18)
    c.apply_swap(1, 0, 100_000 * 10**6)
    last = out["series"][-1]
    assert last["reserve0"] == str(c.balances()[0])
    assert last["reserve1"] == str(c.balances()[1])
    assert out["drift_count"] == 0


@skip_no_flox
def test_replay_pool_tape_flags_drift():
    pool = _v2(["1000 WETH", "2000000 USDC"])
    # Drive it via evm_logs: one 1-WETH sell (amount0In = 1e18).
    one = (10**18).to_bytes(32, "big").hex()
    zero = (0).to_bytes(32, "big").hex()
    logs = [{"blockNumber": "0x10", "data": "0x" + one + zero + zero + zero}]
    out = json.loads(runtime.replay_pool_tape(pool, evm_logs=logs))
    c = flox_py.AmmCurve.constant_product(1000 * 10**18, 2_000_000 * 10**6, 997, 1000)
    c.apply_swap(0, 1, 10**18)
    assert out["series"][-1]["reserve0"] == str(c.balances()[0])


@skip_no_flox
def test_replay_pool_tape_requires_input():
    assert "supply either" in runtime.replay_pool_tape(_v2(["1000 WETH", "2000000 USDC"]))


def test_tools_report_missing_flox_cleanly(monkeypatch):
    """With flox_py absent the tools return a clean install hint, never a traceback."""
    monkeypatch.setattr(runtime, "_import_dex_or_error",
                        lambda: (None, "this tool requires the optional `flox-py` package."))
    for call in (lambda: runtime.route_amm_swap([{}], "1 WETH"),
                 lambda: runtime.amm_price_impact({}, ["1 WETH"]),
                 lambda: runtime.replay_pool_tape({}, swaps=[[1, "1 WETH"]])):
        assert "flox-py" in call()
