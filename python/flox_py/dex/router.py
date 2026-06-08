"""flox_py.dex.router -- multi-pool desk tools over flox.dex pools.

Route a notional to the best venue, and size a depth-aware cross-pool arb. Both work
off the exact curve through ``quote`` (which does not move the pool), so the figures are
wei-exact and the live pools stay put.

    from flox_py import dex

    r = dex.Router([uni, ray, sushi])
    pool, q = r.best("50000 USDC", into="WETH")   # the venue that fills the most

    a = dex.arb(uni, ray)                          # the size that maximises the spread
    a.size, a.profit, a.route
"""
from __future__ import annotations

from decimal import Decimal
from typing import List, Optional, Sequence, Tuple

from . import Amount, Pool, Quote, Token


class Router:
    """A set of pools quoted together: best execution and a comparison table."""

    def __init__(self, pools: Sequence[Pool]):
        self.pools = list(pools)
        if not self.pools:
            raise ValueError("Router needs at least one pool")

    def quotes(self, amount, into=None) -> List[Tuple[Pool, Quote]]:
        """Every pool that can price the swap, as (pool, Quote). Pools that cannot
        (an unknown token, a vanishing output) are skipped rather than raising."""
        out = []
        for p in self.pools:
            try:
                out.append((p, p.quote(amount, into)))
            except ValueError:
                continue
        return out

    def best(self, amount, into=None) -> Tuple[Pool, Quote]:
        """The (pool, Quote) that returns the most output for `amount`."""
        qs = self.quotes(amount, into)
        if not qs:
            raise ValueError("no pool in the router could price this swap")
        return max(qs, key=lambda pq: pq[1].out.wei)

    def table(self, amount, into=None):
        """A comparison table: output and price impact per venue, best first."""
        rows = [{"venue": p.venue, "out": repr(q.out), "out_human": float(q.out.human),
                 "impact_pct": float(q.price_impact * 100)}
                for p, q in sorted(self.quotes(amount, into),
                                   key=lambda pq: pq[1].out.wei, reverse=True)]
        try:
            import pandas as pd
            return pd.DataFrame(rows)
        except ImportError:
            return rows


class Arb:
    """A cross-pool arbitrage: the input size, the token1 profit, and the route."""

    def __init__(self, size: Amount, profit: Amount, route: Optional[Tuple[str, str]]):
        self.size = size
        self.profit = profit
        self.route = route        # (buy_venue, sell_venue), or None when there is no edge

    @property
    def profitable(self) -> bool:
        return self.profit.wei > 0

    def __repr__(self):
        if not self.profitable:
            return "Arb(no edge)"
        return (f"Arb(size={self.size!r}, profit={self.profit!r}, "
                f"route={self.route[0]}->{self.route[1]})")


def arb(pool_a: Pool, pool_b: Pool) -> Arb:
    """Depth-aware sizing of the arb between two pools on the same pair.

    Spends token1 on the cheaper pool to buy token0, sells that token0 on the dearer
    pool for token1; returns the input size that maximises the token1 profit. Profit is
    concave in size, so an interior optimum is found by ternary search on the wei and a
    short exact scan around it. Both legs are priced at the pools' current state (an
    atomic two-leg arb), so the live pools are untouched.
    """
    t0, t1 = pool_a.token0, pool_a.token1
    if {pool_b.token0.symbol, pool_b.token1.symbol} != {t0.symbol, t1.symbol}:
        raise ValueError(f"pools trade different pairs: {t0.symbol}/{t1.symbol} vs "
                         f"{pool_b.token0.symbol}/{pool_b.token1.symbol}")

    # Cheaper-to-buy-token0 pool is the one with the lower token1-per-token0 price.
    pa, pb = pool_a.price(t0, t1), pool_b.price(t0, t1)
    if pa == pb:
        return Arb(Amount(t1, 0), Amount(t1, 0), None)
    buy, sell = (pool_a, pool_b) if pa < pb else (pool_b, pool_a)

    bi1, bi0 = buy._index(t1), buy._index(t0)
    si0, si1 = sell._index(t0), sell._index(t1)

    def profit_wei(x: int) -> int:
        if x <= 0:
            return 0
        t0_out = buy._c.amount_out(bi1, bi0, x)
        if t0_out == 0:
            return 0
        return sell._c.amount_out(si0, si1, t0_out) - x

    # Upper bound: the buy pool's whole token1 reserve is past any profitable size.
    hi = int(buy._c.balances()[bi1])
    if hi <= 0:
        return Arb(Amount(t1, 0), Amount(t1, 0), None)
    lo = 0
    while hi - lo > 2:
        m1 = lo + (hi - lo) // 3
        m2 = hi - (hi - lo) // 3
        if profit_wei(m1) < profit_wei(m2):
            lo = m1
        else:
            hi = m2
    best_x = max(range(lo, hi + 1), key=profit_wei)
    best_p = profit_wei(best_x)
    if best_p <= 0:
        return Arb(Amount(t1, 0), Amount(t1, 0), None)
    return Arb(Amount(t1, best_x), Amount(t1, best_p), (buy.venue, sell.venue))
