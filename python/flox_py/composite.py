"""Composite-condition DSL for multi-symbol multi-TF strategies.

Lets a strategy declare entry / exit logic as a tree built from
indicator handles and comparison operators rather than inlining the
boolean state machine in `on_bar`. Reads bars from the per-(symbol,
timeframe) ring populated by `Strategy.last_n_closed_bars` (W1-T026),
so warmup, multi-symbol, and multi-TF are handled uniformly.

Example::

    from flox_py import Strategy
    from flox_py.composite import when

    H4_NS = 4 * 3600 * 1_000_000_000
    M5_NS = 5 * 60 * 1_000_000_000
    TIME = 0  # BarType.Time

    class TrendFollow(Strategy):
        def on_bar(self, ctx, bar):
            entry = (
                when(self, ctx.symbol_id, TIME, H4_NS).ema(50)
                > when(self, ctx.symbol_id, TIME, H4_NS).ema(200)
            ) & (
                when(self, ctx.symbol_id, TIME, M5_NS).rsi(14) < 30
            )
            if entry.is_ready() and entry.value():
                self.emit_market_buy(ctx.symbol_id, 0.01)

The handle (`when(...)`) caches per-(symbol, tf) the underlying
strategy reference; an indicator on it (`ema`, `sma`, `rsi`) returns a
node that lazily pulls bars from the ring and computes the value.
Comparison operators (`<`, `>`, `==`, ...) wrap the node in a
condition; condition operators (`&`, `|`, `~`) build a tree.
"""
from __future__ import annotations

from typing import Any, List

# BarType numeric values — keep them in sync with include/flox/aggregator/bar.h.
TIME_BARS = 0
TICK_BARS = 1
VOLUME_BARS = 2
RENKO_BARS = 3
RANGE_BARS = 4
HEIKIN_ASHI_BARS = 5
BPS_RANGE_BARS = 6


class _Node:
    """Base for any expression that resolves to a number or bool."""
    def is_ready(self) -> bool: raise NotImplementedError
    def value(self) -> float: raise NotImplementedError

    def __lt__(self, other: Any) -> "_Condition":
        return _Compare(self, _wrap_const(other), lambda a, b: a < b)
    def __le__(self, other: Any) -> "_Condition":
        return _Compare(self, _wrap_const(other), lambda a, b: a <= b)
    def __gt__(self, other: Any) -> "_Condition":
        return _Compare(self, _wrap_const(other), lambda a, b: a > b)
    def __ge__(self, other: Any) -> "_Condition":
        return _Compare(self, _wrap_const(other), lambda a, b: a >= b)
    def __eq__(self, other: Any) -> "_Condition":  # type: ignore[override]
        return _Compare(self, _wrap_const(other), lambda a, b: a == b)
    def __ne__(self, other: Any) -> "_Condition":  # type: ignore[override]
        return _Compare(self, _wrap_const(other), lambda a, b: a != b)


class _Const(_Node):
    def __init__(self, v: float) -> None:
        self._v = float(v)
    def is_ready(self) -> bool: return True
    def value(self) -> float: return self._v


def _wrap_const(x: Any) -> _Node:
    if isinstance(x, _Node):
        return x
    return _Const(float(x))


class _Indicator(_Node):
    """Pulls a window of bars from the strategy's ring and computes a value."""

    def __init__(self, strategy: Any, symbol_id: int, bar_type: int, param: int,
                 period: int, kind: str) -> None:
        self._s = strategy
        self._sym = symbol_id
        self._bt = bar_type
        self._param = param
        self._n = period
        self._kind = kind

    def _bars(self) -> List[Any]:
        return self._s.last_n_closed_bars(self._sym, self._bt, self._param, self._needed())

    def _needed(self) -> int:
        # RSI needs n+1 closes to compute n-period changes; rolling
        # averages need n. Keep the smaller useful window for warmup.
        return self._n + 1 if self._kind == "rsi" else self._n

    def is_ready(self) -> bool:
        return len(self._bars()) >= self._needed()

    def value(self) -> float:
        bars = self._bars()
        if len(bars) < self._needed():
            return float("nan")
        closes = [b["close"] for b in bars[-self._n - (1 if self._kind == "rsi" else 0):]]
        kind = self._kind
        if kind == "sma":
            return sum(closes[-self._n:]) / self._n
        if kind == "ema":
            alpha = 2.0 / (self._n + 1.0)
            v = closes[0]
            for c in closes[1:]:
                v = alpha * c + (1 - alpha) * v
            return v
        if kind == "rsi":
            gains = 0.0
            losses = 0.0
            for i in range(1, len(closes)):
                d = closes[i] - closes[i - 1]
                if d >= 0:
                    gains += d
                else:
                    losses -= d
            avg_g = gains / self._n
            avg_l = losses / self._n
            if avg_l == 0:
                return 100.0
            rs = avg_g / avg_l
            return 100.0 - (100.0 / (1.0 + rs))
        if kind == "close":
            return closes[-1]
        raise ValueError(f"unknown indicator kind: {kind}")


class _TfHandle:
    """Returned by `when(strategy, symbol_id, bar_type, param)`. Each
    method on it produces an indicator node bound to that timeframe."""

    def __init__(self, strategy: Any, symbol_id: int, bar_type: int, param: int) -> None:
        self._s = strategy
        self._sym = symbol_id
        self._bt = bar_type
        self._param = param

    def sma(self, period: int) -> _Indicator:
        return _Indicator(self._s, self._sym, self._bt, self._param, period, "sma")
    def ema(self, period: int) -> _Indicator:
        return _Indicator(self._s, self._sym, self._bt, self._param, period, "ema")
    def rsi(self, period: int) -> _Indicator:
        return _Indicator(self._s, self._sym, self._bt, self._param, period, "rsi")
    def close(self) -> _Indicator:
        return _Indicator(self._s, self._sym, self._bt, self._param, 1, "close")


def when(strategy: Any, symbol_id: int, bar_type: int, param: int) -> _TfHandle:
    """Entry point. Returns a handle bound to (strategy, symbol_id,
    bar_type, param); call indicator methods on it to produce nodes."""
    return _TfHandle(strategy, symbol_id, bar_type, param)


class _Condition:
    """A boolean expression. Combine with `&`, `|`, `~`."""

    def is_ready(self) -> bool: raise NotImplementedError
    def value(self) -> bool: raise NotImplementedError

    def __and__(self, other: "_Condition") -> "_Condition":
        return _And(self, other)
    def __or__(self, other: "_Condition") -> "_Condition":
        return _Or(self, other)
    def __invert__(self) -> "_Condition":
        return _Not(self)


class _Compare(_Condition):
    def __init__(self, lhs: _Node, rhs: _Node, op) -> None:
        self._l = lhs
        self._r = rhs
        self._op = op

    def is_ready(self) -> bool:
        return self._l.is_ready() and self._r.is_ready()

    def value(self) -> bool:
        return bool(self._op(self._l.value(), self._r.value()))


class _And(_Condition):
    def __init__(self, l: _Condition, r: _Condition) -> None:
        self._l = l
        self._r = r
    def is_ready(self) -> bool:
        return self._l.is_ready() and self._r.is_ready()
    def value(self) -> bool:
        return bool(self._l.value() and self._r.value())


class _Or(_Condition):
    def __init__(self, l: _Condition, r: _Condition) -> None:
        self._l = l
        self._r = r
    def is_ready(self) -> bool:
        return self._l.is_ready() and self._r.is_ready()
    def value(self) -> bool:
        return bool(self._l.value() or self._r.value())


class _Not(_Condition):
    def __init__(self, inner: _Condition) -> None:
        self._inner = inner
    def is_ready(self) -> bool: return self._inner.is_ready()
    def value(self) -> bool: return not bool(self._inner.value())


# ---------------------------------------------------------------------------
# Indicator-grid sugar: instantiate the same indicator across a cross-product
# of symbols and timeframes in one declaration.
# ---------------------------------------------------------------------------

class _IndicatorGrid:
    """Dict-like view over a (symbol_id, timeframe) → indicator cross-product.

    Use either `grid[(symbol, param)]` or `grid[symbol, param]` to look up
    a single cell. Iterate to walk every cell.
    """

    def __init__(self, cells: dict) -> None:
        self._cells = cells  # (symbol, bar_type, param) -> _Indicator

    def __getitem__(self, key):
        # Accept (symbol, param) sugar by defaulting bar_type to TIME_BARS.
        if isinstance(key, tuple) and len(key) == 2:
            symbol, param = key
            return self._cells[(symbol, TIME_BARS, param)]
        if isinstance(key, tuple) and len(key) == 3:
            return self._cells[key]
        raise KeyError(f"unrecognized key shape: {key!r}")

    def __iter__(self):
        for k, v in self._cells.items():
            yield k, v

    def __len__(self) -> int:
        return len(self._cells)


class _GridBuilder:
    """Returned by `grid(strategy, symbols, timeframes)`. Each indicator
    method on it (`sma`, `ema`, `rsi`, `close`) instantiates one
    `_Indicator` per (symbol, timeframe) cell and bundles them in
    a dict-like grid."""

    def __init__(self, strategy, symbols, timeframes) -> None:
        self._s = strategy
        self._symbols = list(symbols)
        # Each timeframe may be either an int (interpreted as a Time-bar
        # interval in nanoseconds) or a `(bar_type, param)` tuple.
        self._tfs: list = []
        for tf in timeframes:
            if isinstance(tf, tuple):
                self._tfs.append((int(tf[0]), int(tf[1])))
            else:
                self._tfs.append((TIME_BARS, int(tf)))

    def _build(self, period: int, kind: str) -> _IndicatorGrid:
        cells = {}
        for sym in self._symbols:
            for bt, param in self._tfs:
                cells[(sym, bt, param)] = _Indicator(self._s, sym, bt, param, period, kind)
        return _IndicatorGrid(cells)

    def sma(self, period: int) -> _IndicatorGrid: return self._build(period, "sma")
    def ema(self, period: int) -> _IndicatorGrid: return self._build(period, "ema")
    def rsi(self, period: int) -> _IndicatorGrid: return self._build(period, "rsi")
    def close(self) -> _IndicatorGrid: return self._build(1, "close")


def grid(strategy, symbols, timeframes) -> _GridBuilder:
    """Returns a builder for a cross-product indicator grid.

    Example::

        ema50 = grid(self, [BTC, ETH], [H4_NS, M5_NS]).ema(50)
        ema50[(BTC, H4_NS)].value()
        for (sym, bt, param), ind in ema50:
            ...
    """
    return _GridBuilder(strategy, symbols, timeframes)
