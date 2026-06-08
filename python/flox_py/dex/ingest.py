"""flox_py.dex.ingest -- go from a chain address to a Pool or a backtest Tape.

These adapters read the pool state (or its swap history) over a JSON-RPC endpoint and
hand back the comfort-layer objects. The RPC is injected as a plain callable
``rpc(method, params) -> result`` (the decoded ``result`` field), so a live provider, a
cached fixture, or a test double all work the same way and CI never touches the network.

    from flox_py import dex

    pool = dex.from_evm_pool("0x88e6...5640", usdc, weth, rpc, kind="v3", fee="0.05%")
    tape = dex.tape_from_evm_logs(pool, rpc, from_block=19_000_000, to_block=19_000_500)
    bt = tape.replay()
"""
from __future__ import annotations

from typing import Callable, List, Optional, Sequence

from . import Amount, Token, UniswapV2, UniswapV3
from .tape import Tape

Rpc = Callable[[str, list], object]

# Function selectors (keccak4 of the signature). Constant, so no ABI library needed.
_V2_GET_RESERVES = "0x0902f1ac"   # getReserves() -> (uint112,uint112,uint32)
_V3_SLOT0 = "0x3850c7bd"          # slot0() -> (uint160 sqrtPriceX96, int24 tick, ...)
_V3_LIQUIDITY = "0x1a686502"      # liquidity() -> uint128

# topic0 of UniswapV2 Swap(address,uint256,uint256,uint256,uint256,address).
_V2_SWAP_TOPIC = "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822"


def _eth_call(rpc: Rpc, to: str, data: str, block: str = "latest") -> str:
    return rpc("eth_call", [{"to": to, "data": data}, block])


def _words(hexstr: str) -> List[int]:
    h = hexstr[2:] if hexstr.startswith("0x") else hexstr
    return [int(h[k:k + 64], 16) for k in range(0, len(h), 64)]


def _reserves_str(wei: int, token: Token) -> str:
    # A human "NUMBER SYMBOL" reserve that round-trips back to `wei` exactly:
    # Decimal scaling is lossless, and the venue ctor multiplies it straight back.
    return f"{token.to_human(wei)} {token.symbol}"


def from_evm_pool(address: str, token0: Token, token1: Token, rpc: Rpc,
                  kind: str = "v2", fee="0.30%") -> object:
    """Read an EVM AMM pool's current state into a Pool.

    kind="v2" reads getReserves(); kind="v3" reads slot0() + liquidity(). token0/token1
    must be passed in the pool's on-chain order (they carry the decimals the chain omits).
    """
    if kind == "v2":
        w = _words(_eth_call(rpc, address, _V2_GET_RESERVES))
        r0, r1 = w[0], w[1]
        return UniswapV2(token0, token1,
                         reserves=(_reserves_str(r0, token0), _reserves_str(r1, token1)),
                         fee=fee)
    if kind == "v3":
        sqrt_x96 = _words(_eth_call(rpc, address, _V3_SLOT0))[0]
        liq = _words(_eth_call(rpc, address, _V3_LIQUIDITY))[0]
        return UniswapV3(token0, token1, sqrt_x96, liq, fee=fee)
    raise ValueError(f"unknown pool kind {kind!r} (use 'v2' or 'v3')")


def tape_from_evm_logs(pool, rpc: Rpc, from_block: int, to_block: int,
                       address: Optional[str] = None) -> Tape:
    """Fetch the pool's Uniswap v2 Swap logs over a block range and build a Tape.

    `pool` supplies the token order and the replay's starting state; its address is used
    unless `address` is given.
    """
    addr = address or getattr(pool, "address", None)
    if addr is None:
        raise ValueError("no pool address: pass address=...")
    logs = rpc("eth_getLogs", [{
        "address": addr,
        "fromBlock": hex(from_block),
        "toBlock": hex(to_block),
        "topics": [_V2_SWAP_TOPIC],
    }])
    return Tape.from_evm_logs(pool, list(logs))


def tape_from_solana(pool, signatures: Sequence[str], rpc: Rpc,
                     vault0: str, vault1: str) -> Tape:
    """Build a Tape from Solana swap transactions via vault balance deltas.

    For each signature, getTransaction's pre/postTokenBalances on the pool's two vault
    accounts give the exact amount in and out and the direction -- venue-agnostic and
    exact, no instruction decoding needed. vault0/vault1 are the token0/token1 vaults.
    """
    tape = Tape(pool)
    for sig in signatures:
        tx = rpc("getTransaction", [sig, {"encoding": "jsonParsed",
                                          "maxSupportedTransactionVersion": 0}])
        if not tx:
            continue
        meta = tx["meta"]
        keys = _account_keys(tx)
        pre = {b["accountIndex"]: int(b["uiTokenAmount"]["amount"])
               for b in meta.get("preTokenBalances", [])}
        post = {b["accountIndex"]: int(b["uiTokenAmount"]["amount"])
                for b in meta.get("postTokenBalances", [])}
        d0 = _vault_delta(keys, pre, post, vault0)
        d1 = _vault_delta(keys, pre, post, vault1)
        if d0 is None or d1 is None:
            continue
        # The vault that gained tokens received the input; the other paid the output.
        if d0 > 0 and d1 < 0:
            ts = int(tx.get("blockTime", 0))
            tape.swap_wei(ts, d0, base_for_quote=True)
        elif d1 > 0 and d0 < 0:
            ts = int(tx.get("blockTime", 0))
            tape.swap_wei(ts, d1, base_for_quote=False)
    return tape


def _account_keys(tx: dict) -> List[str]:
    msg = tx["transaction"]["message"]
    keys = msg.get("accountKeys", [])
    return [k["pubkey"] if isinstance(k, dict) else k for k in keys]


def _vault_delta(keys: List[str], pre: dict, post: dict, vault: str) -> Optional[int]:
    try:
        idx = keys.index(vault)
    except ValueError:
        return None
    if idx not in pre and idx not in post:
        return None
    return post.get(idx, 0) - pre.get(idx, 0)
