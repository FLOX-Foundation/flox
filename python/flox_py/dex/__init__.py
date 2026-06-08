"""flox_py.dex -- the comfort layer over the exact AMM curves.

The compiled ``flox_py.AmmCurve`` is exact to the wei but low-level: amounts are bare
``int`` wei and swap directions are token indices, so a mis-indexed call returns a
silent wrong answer. This package wraps it in a symbol- and decimals-aware surface:

    from flox_py import dex

    weth, usdc = dex.Token("WETH", 18), dex.Token("USDC", 6)
    pool = dex.UniswapV2(weth, usdc, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
    pool.spot_price                 # Decimal('2000.00') -- token1 per token0
    pool.quote("50000 USDC")        # Quote(out=24.81 WETH, impact=0.41%, ...)

Human units go in and out, the exact wei math runs underneath, and misuse raises
instead of returning a quiet zero.
"""
from __future__ import annotations

from decimal import Decimal, getcontext
from typing import Iterable, List, Optional, Sequence, Tuple, Union

from .. import AmmCurve  # the compiled, exact curve

getcontext().prec = 60  # plenty for 256-bit values rendered as Decimal

Q96 = 2 ** 96
Numeric = Union[int, float, str, Decimal]


# ── Token / Amount ────────────────────────────────────────────────────

class Token:
    """A token: a symbol and its on-chain decimals."""

    __slots__ = ("symbol", "decimals")

    def __init__(self, symbol: str, decimals: int):
        self.symbol = symbol
        self.decimals = int(decimals)

    def to_wei(self, human: Numeric) -> int:
        return int((Decimal(str(human)) * (10 ** self.decimals)).to_integral_value())

    def to_human(self, wei: int) -> Decimal:
        return Decimal(int(wei)) / (10 ** self.decimals)

    def amount(self, human: Numeric) -> "Amount":
        return Amount(self, self.to_wei(human))

    def __eq__(self, other):
        return isinstance(other, Token) and (self.symbol, self.decimals) == (other.symbol,
                                                                             other.decimals)

    def __hash__(self):
        return hash((self.symbol, self.decimals))

    def __repr__(self):
        return f"Token({self.symbol!r}, {self.decimals})"


class Amount:
    """A quantity bound to a token. Carries exact wei, renders human."""

    __slots__ = ("token", "wei")

    def __init__(self, token: Token, wei: int):
        self.token = token
        self.wei = int(wei)

    @classmethod
    def parse(cls, spec: Union["Amount", str], tokens: dict) -> "Amount":
        if isinstance(spec, Amount):
            return spec
        if not isinstance(spec, str):
            raise TypeError(f"amount must be 'NUMBER SYMBOL' or an Amount, got {spec!r}")
        parts = spec.replace("_", "").split()
        if len(parts) != 2:
            raise ValueError(f"amount must be 'NUMBER SYMBOL', got {spec!r}")
        num, sym = parts
        if sym not in tokens:
            raise ValueError(f"unknown token {sym!r}; this pool holds {sorted(tokens)}")
        return tokens[sym].amount(num)

    @property
    def human(self) -> Decimal:
        return self.token.to_human(self.wei)

    def __repr__(self):
        return f"{self.human.normalize():f} {self.token.symbol}"


class Quote:
    """The result of pricing a swap: human out, impact, fee, and the exact wei."""

    __slots__ = ("amount_in", "out", "fee", "price_impact", "avg_price")

    def __init__(self, amount_in: Amount, out: Amount, fee: Amount, price_impact: Decimal,
                 avg_price: Decimal):
        self.amount_in = amount_in
        self.out = out
        self.fee = fee
        self.price_impact = price_impact
        self.avg_price = avg_price

    @property
    def exact_wei(self) -> int:
        return self.out.wei

    def __repr__(self):
        return (f"Quote(out={self.out!r}, impact={self.price_impact:.3%}, "
                f"avg_px={self.avg_price:.4f}, fee={self.fee!r})")


# ── Pool ──────────────────────────────────────────────────────────────

class Pool:
    """A two-token AMM pool. token0/token1 match the curve's indices 0/1."""

    venue = "amm"

    def __init__(self, token0: Token, token1: Token, curve: AmmCurve):
        self.token0 = token0
        self.token1 = token1
        self._c = curve
        self._tokens = {token0.symbol: token0, token1.symbol: token1}

    # -- direction helpers --
    def _index(self, token: Token) -> int:
        if token == self.token0:
            return 0
        if token == self.token1:
            return 1
        raise ValueError(f"{token.symbol} is not in this pool ({self.token0.symbol}/"
                         f"{self.token1.symbol})")

    def _other(self, token: Token) -> Token:
        return self.token1 if token == self.token0 else self.token0

    def _resolve(self, amount, into):
        amt = Amount.parse(amount, self._tokens)
        if into is None:
            out_tok = self._other(amt.token)
        else:
            out_tok = into if isinstance(into, Token) else self._tokens.get(into)
            if out_tok is None:
                raise ValueError(f"unknown out-token {into!r}")
        return amt, out_tok

    # -- pricing --
    def quote(self, amount, into=None) -> Quote:
        amt, out_tok = self._resolve(amount, into)
        i, j = self._index(amt.token), self._index(out_tok)
        out_wei = self._c.amount_out(i, j, amt.wei)
        # Guard-rail: a non-zero input into a funded pool must not vanish to ~0.
        if amt.wei > 0 and out_wei == 0 and min(self._c.balances()) > 0:
            raise ValueError(
                f"swap of {amt!r} -> {out_tok.symbol} returned 0 -- check token order / "
                f"decimals (amount is {amt.token.symbol}, {amt.token.decimals} decimals)")
        out = Amount(out_tok, out_wei)
        avg = (out.human / amt.human) if amt.human > 0 else Decimal(0)
        mid = self._mid(i, j)
        impact = (1 - avg / mid) if mid > 0 else Decimal(0)
        fee = Amount(amt.token, self._fee_wei(amt.wei))
        return Quote(amt, out, fee, impact, avg)

    def swap(self, amount, into=None) -> Quote:
        """Like quote(), but moves the pool."""
        amt, out_tok = self._resolve(amount, into)
        i, j = self._index(amt.token), self._index(out_tok)
        out_wei = self._c.apply_swap(i, j, amt.wei)
        return Quote(amt, Amount(out_tok, out_wei), Amount(amt.token, self._fee_wei(amt.wei)),
                     Decimal(0), (Amount(out_tok, out_wei).human / amt.human) if amt.human else Decimal(0))

    # -- state --
    @property
    def reserves(self) -> Tuple[Amount, Amount]:
        b = self._c.balances()
        return Amount(self.token0, b[0]), Amount(self.token1, b[1])

    @property
    def spot_price(self) -> Decimal:
        """Marginal price of token0 quoted in token1 (e.g. WETH priced in USDC)."""
        return self._mid(0, 1)

    def price(self, of: Token, in_: Token) -> Decimal:
        return self._mid(self._index(of), self._index(in_))

    def depth(self, sizes: Sequence[Union[str, Amount]]):
        """A slippage table: for each size, the realized average price and impact."""
        rows = []
        for s in sizes:
            q = self.quote(s)
            rows.append({"in": repr(q.amount_in), "out": repr(q.out),
                         "avg_price": float(q.avg_price), "impact_pct": float(q.price_impact * 100)})
        try:
            import pandas as pd  # optional
            return pd.DataFrame(rows)
        except ImportError:
            return rows

    def clone(self) -> "Pool":
        p = object.__new__(type(self))
        p.__dict__.update(self.__dict__)  # keep venue-specific fee fields
        p._c = self._c.clone()
        return p

    # -- mid price (token j per token i), exact-ish via the curve --
    def _mid(self, i: int, j: int) -> Decimal:
        # A constant-product / Raydium mid is the reserve ratio; a CLMM mid is the sqrt
        # price squared. Both derived from the exact state.
        sp = self._c.sqrt_price()
        if sp is not None:
            # v3: (sqrtP/2^96)^2 = token1_wei per token0_wei.
            raw = (Decimal(sp) / Q96) ** 2
            t0, t1 = self.token0, self.token1
            price0_in_1 = raw * (Decimal(10) ** t0.decimals) / (Decimal(10) ** t1.decimals)
            return price0_in_1 if (i, j) == (0, 1) else (1 / price0_in_1 if price0_in_1 else Decimal(0))
        b = self._c.balances()
        if b[i] == 0:
            return Decimal(0)
        hi = self.token0.to_human(b[i]) if i == 0 else self.token1.to_human(b[i])
        hj = self.token0.to_human(b[j]) if j == 0 else self.token1.to_human(b[j])
        return hj / hi if hi > 0 else Decimal(0)

    def _fee_wei(self, amount_in_wei: int) -> int:
        return 0  # overridden per venue

    def __repr__(self):
        r0, r1 = self.reserves
        return (f"<{self.venue} {self.token0.symbol}/{self.token1.symbol}  "
                f"px={self.spot_price:.2f}  reserves=({r0!r}, {r1!r})>")

    def _repr_html_(self):
        r0, r1 = self.reserves
        return (f"<table><tr><th>{self.venue}</th><th>{self.token0.symbol}/"
                f"{self.token1.symbol}</th></tr>"
                f"<tr><td>spot</td><td>{self.spot_price:.4f} {self.token1.symbol}/"
                f"{self.token0.symbol}</td></tr>"
                f"<tr><td>reserves</td><td>{r0!r} &nbsp; {r1!r}</td></tr></table>")


# ── venue helpers ─────────────────────────────────────────────────────

def _pct(spec: Union[str, float, Decimal]) -> Decimal:
    if isinstance(spec, str):
        return Decimal(spec.strip().rstrip("%")) / 100
    return Decimal(str(spec))


class UniswapV2(Pool):
    venue = "UniswapV2"

    def __init__(self, token0: Token, token1: Token, reserves, fee: Union[str, float] = "0.30%",
                 fee_den: int = 1000):
        r0, r1 = (Amount.parse(r, {token0.symbol: token0, token1.symbol: token1})
                  for r in reserves)
        f = _pct(fee)                            # 0.30% -> keep 99.70%
        self._fee_num = int((1 - f) * fee_den)
        self._fee_den = fee_den
        super().__init__(token0, token1,
                         AmmCurve.constant_product(r0.wei, r1.wei, self._fee_num, self._fee_den))

    def _fee_wei(self, amount_in_wei: int) -> int:
        return amount_in_wei - amount_in_wei * self._fee_num // self._fee_den


class RaydiumCp(Pool):
    venue = "RaydiumCp"

    def __init__(self, token0: Token, token1: Token, reserves, trade_fee: Union[str, float] = "0.25%",
                 creator_fee: Union[str, float] = "0%", creator_fee_on_input: bool = True):
        r0, r1 = (Amount.parse(r, {token0.symbol: token0, token1.symbol: token1})
                  for r in reserves)
        self._trade = int(_pct(trade_fee) * 1_000_000)
        self._creator = int(_pct(creator_fee) * 1_000_000)
        super().__init__(token0, token1,
                         AmmCurve.raydium_cp(r0.wei, r1.wei, self._trade, self._creator,
                                             creator_fee_on_input))

    def _fee_wei(self, amount_in_wei: int) -> int:
        return -(-amount_in_wei * self._trade // 1_000_000)  # ceil-div, trade fee


class UniswapV3(Pool):
    venue = "UniswapV3"

    def __init__(self, token0: Token, token1: Token, sqrt_price_x96: int, liquidity: int,
                 fee: Union[str, float] = "0.05%", ticks: Iterable[Tuple[int, int]] = ()):
        self._fee_pips = int(_pct(fee) * 1_000_000)
        super().__init__(token0, token1,
                         AmmCurve.uniswap_v3(int(sqrt_price_x96), int(liquidity), self._fee_pips,
                                             [(int(s), int(n)) for s, n in ticks]))

    @classmethod
    def from_price(cls, token0: Token, token1: Token, price: Numeric, liquidity: int,
                   fee: Union[str, float] = "0.05%", ticks: Iterable[Tuple[int, int]] = ()):
        """Build a v3 pool from a human price (token1 per token0) instead of a raw Q64.96."""
        # price = token1_human / token0_human; raw = token1_wei / token0_wei.
        raw = Decimal(str(price)) * (Decimal(10) ** token1.decimals) / (Decimal(10) ** token0.decimals)
        sqrt_x96 = int((raw.sqrt() * Q96).to_integral_value())
        return cls(token0, token1, sqrt_x96, liquidity, fee, ticks)

    @property
    def sqrt_price(self) -> Optional[int]:
        return self._c.sqrt_price()

    @property
    def liquidity(self) -> Optional[int]:
        return self._c.liquidity()

    def _fee_wei(self, amount_in_wei: int) -> int:
        return amount_in_wei * self._fee_pips // 1_000_000


__all__ = ["Token", "Amount", "Quote", "Pool", "UniswapV2", "RaydiumCp", "UniswapV3"]
