"""flox_py.dex.tape -- replay a recorded pool history into a backtest.

A pool-state tape is a delta log; the pool's state is derived by replaying the swaps
through the exact curve. This turns the low-level PoolTape / PoolReplay binding into a
quant's workflow: build a tape from swaps or decoded chain logs, then ``replay()`` into
a table of price / reserves / LP value / impermanent loss / drift over time.

    from flox_py import dex

    pool = dex.UniswapV2(weth, usdc, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    tape = dex.Tape(pool)
    tape.from_swaps([(1, "80000 USDC"), (2, "80000 USDC")])
    bt = tape.replay()          # a DataFrame (or list of rows without pandas)
"""
from __future__ import annotations

from decimal import Decimal
from typing import List, Optional, Sequence, Tuple, Union

from . import Amount, Pool, Token

# A swap event: (ts, in_token_index, amount_in_wei, out_token_index).
# A checkpoint event: (ts, reserve0_wei, reserve1_wei).


class Tape:
    """A recorded pool history -- swaps and optional checkpoints -- to replay."""

    def __init__(self, pool: Pool):
        self.pool = pool
        self._swaps: List[Tuple[int, int, int, int]] = []
        self._checkpoints: dict = {}  # ts -> (r0_wei, r1_wei)

    # -- building --
    def swap(self, ts: int, amount, into=None) -> "Tape":
        amt, out_tok = self.pool._resolve(amount, into)
        i, j = self.pool._index(amt.token), self.pool._index(out_tok)
        self._swaps.append((int(ts), i, amt.wei, j))
        return self

    def swap_wei(self, ts: int, amount_in_wei: int, base_for_quote: bool) -> "Tape":
        """A swap straight in wei (e.g. from a decoded log). base_for_quote sells token0."""
        i, j = (0, 1) if base_for_quote else (1, 0)
        self._swaps.append((int(ts), i, int(amount_in_wei), j))
        return self

    def checkpoint(self, ts: int, reserve0, reserve1) -> "Tape":
        self._checkpoints[int(ts)] = (self._to_wei(reserve0, self.pool.token0),
                                      self._to_wei(reserve1, self.pool.token1))
        return self

    @staticmethod
    def _to_wei(value, token: Token) -> int:
        """A reserve as an Amount, a "NUMBER SYMBOL" string, or a bare number in `token`."""
        if isinstance(value, Amount):
            return value.wei
        if isinstance(value, str) and any(c.isalpha() and c not in "eE" for c in value):
            return Amount.parse(value, {token.symbol: token}).wei
        return token.to_wei(value)

    def from_swaps(self, swaps: Sequence) -> "Tape":
        """swaps: a list of (ts, amount, into=None) human swaps."""
        for s in swaps:
            ts, amount = s[0], s[1]
            into = s[2] if len(s) > 2 else None
            self.swap(ts, amount, into)
        return self

    @classmethod
    def from_evm_logs(cls, pool: Pool, swap_logs: Sequence[dict]) -> "Tape":
        """Build from decoded Uniswap v2 Swap logs.

        Each log is the eth_getLogs dict; the data word layout is
        (amount0In, amount1In, amount0Out, amount1Out). token0/token1 of `pool` must
        match the pool's on-chain token order.
        """
        tape = cls(pool)
        for lg in swap_logs:
            data = lg["data"][2:] if lg["data"].startswith("0x") else lg["data"]
            words = [int(data[k:k + 64], 16) for k in range(0, len(data), 64)]
            a0in, a1in = words[0], words[1]
            ts = int(lg.get("blockNumber", lg.get("ts", 0)), 16) if isinstance(
                lg.get("blockNumber", lg.get("ts", 0)), str) else int(lg.get("blockNumber",
                                                                              lg.get("ts", 0)))
            if a0in > 0:
                tape.swap_wei(ts, a0in, base_for_quote=True)
            elif a1in > 0:
                tape.swap_wei(ts, a1in, base_for_quote=False)
        return tape

    # -- replay --
    def replay(self):
        """Replay through the exact curve, returning a table over time.

        Columns: ts, price (token0 in token1), reserve0, reserve1, lp_value (in token1),
        il (impermanent loss vs holding the starting mix), drift (a checkpoint that
        disagreed with the replayed state).
        """
        work = self.pool.clone()
        t0, t1 = self.pool.token0, self.pool.token1
        b0 = work._c.balances()
        start0, start1 = t0.to_human(b0[0]), t1.to_human(b0[1])

        rows = []
        events = ([("swap", *s) for s in self._swaps]
                  + [("checkpoint", ts, r0, r1) for ts, (r0, r1) in self._checkpoints.items()])
        events.sort(key=lambda e: e[1])  # by ts

        drift_count = 0
        for ev in events:
            if ev[0] == "swap":
                _, ts, i, amt_wei, j = ev
                work._c.apply_swap(i, j, amt_wei)
                bal = work._c.balances()
                price = work.spot_price
                r0h, r1h = t0.to_human(bal[0]), t1.to_human(bal[1])
                lp_value = r0h * price + r1h
                hodl = start0 * price + start1
                il = (lp_value / hodl - 1) if hodl > 0 else Decimal(0)
                rows.append({"ts": ts, "price": float(price), "reserve0": int(bal[0]),
                             "reserve1": int(bal[1]), "lp_value": float(lp_value),
                             "il": float(il), "drift": False, "trade": True})
            else:
                _, ts, r0, r1 = ev
                bal = work._c.balances()
                mismatch = (bal[0] != r0 or bal[1] != r1)
                drift_count += int(mismatch)
                rows.append({"ts": ts, "price": float(work.spot_price), "reserve0": int(bal[0]),
                             "reserve1": int(bal[1]), "lp_value": None, "il": None,
                             "drift": mismatch, "trade": False})

        try:
            import pandas as pd
            df = pd.DataFrame(rows)
            # attrs must stay plain: pandas deepcopies them on most ops, and a live
            # curve handle is not picklable. The final pool is reachable via reserves.
            df.attrs["drift_count"] = drift_count
            df.attrs["final_reserve0"] = int(work._c.balances()[0])
            df.attrs["final_reserve1"] = int(work._c.balances()[1])
            return df
        except ImportError:
            return rows


class LpPosition:
    """A liquidity position pinned to its entry, tracked as the pool moves.

    Construct it against a pool at entry; the entry reserves and price are snapshotted.
    As the *same* pool object is swapped through (or you pass a moved clone to value()),
    ``value()`` and ``impermanent_loss()`` compare the LP's holdings to having simply
    held the entry token mix.

        pos = dex.LpPosition(pool, value="100000 USDC")   # a 100k-USDC-worth stake
        pool.swap("80000 USDC")                            # price moves
        pos.impermanent_loss()                             # < 0
    """

    def __init__(self, pool: Pool, value=None):
        self.pool = pool
        self._t0, self._t1 = pool.token0, pool.token1
        r0, r1 = pool.reserves
        self._entry0, self._entry1 = r0.human, r1.human          # whole-pool reserves at entry
        self._entry_price = pool.spot_price
        pool_value = self._entry0 * self._entry_price + self._entry1
        # `value` is the position's worth in token1 at entry; default = the whole pool.
        if value is None:
            self.share = Decimal(1)
        else:
            self.share = self._value_in_token1(value) / pool_value

    def _value_in_token1(self, value) -> Decimal:
        """Coerce a stake to a token1-denominated human amount.

        Accepts an Amount, a "NUMBER SYMBOL" string, or a bare number (already token1).
        A symboled value must be in token1 -- the pool's value currency.
        """
        if isinstance(value, Amount):
            amt = value
        elif isinstance(value, str) and any(c.isalpha() for c in value):
            amt = Amount.parse(value, {self._t0.symbol: self._t0, self._t1.symbol: self._t1})
        else:
            return Decimal(str(value))
        if amt.token is not self._t1:
            raise ValueError(f"position value must be in {self._t1.symbol} "
                             f"(the value currency), got {amt.token.symbol}")
        return amt.human

    def value(self, at: Optional[Pool] = None) -> Decimal:
        """Position value in token1 at the pool's current spot (or `at` a given pool state)."""
        p = at if at is not None else self.pool
        r0, r1 = p.reserves
        return (r0.human * p.spot_price + r1.human) * self.share

    def hodl_value(self, at: Optional[Pool] = None) -> Decimal:
        """What the entry token mix would be worth now -- the HODL benchmark."""
        p = at if at is not None else self.pool
        return (self._entry0 * p.spot_price + self._entry1) * self.share

    def impermanent_loss(self, at: Optional[Pool] = None) -> Decimal:
        """LP value vs HODL of the entry mix, at the current price.

        Negative when the LP underperforms holding (the usual case after a price move);
        zero when the price has not moved since entry.
        """
        hodl = self.hodl_value(at)
        return (self.value(at) / hodl - 1) if hodl > 0 else Decimal(0)
