"""CCXT live broker for FLOX.

``CcxtBroker`` wraps a ``ccxt.pro`` exchange and gives a strategy a
single entry point for both market data (trades, L2 books, order
updates) and order routing. The strategy's ``self.market_buy(...)`` /
``self.limit_sell(...)`` / ``self.stop_market(...)`` / etc. calls
translate into real ``create_*_order`` calls on the underlying ccxt
exchange.

Quick start
-----------

.. code-block:: python

    import asyncio
    import flox_py as flox
    from flox_py.ccxt import CcxtBroker

    class SMA(flox.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.fast = flox.SMA(10)
            self.slow = flox.SMA(30)

        def on_trade(self, ctx, trade):
            f = self.fast.update(trade.price)
            s = self.slow.update(trade.price)
            if f is None or s is None:
                return
            if f > s and ctx.is_flat():
                self.market_buy(0.001)
            elif f < s and ctx.is_long():
                self.market_sell(0.001)

    async def main():
        async with CcxtBroker("binance") as broker:
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(SMA([btc]))
            await broker.run(streams=("trades", "book", "orders"))

    asyncio.run(main())

What the broker does
--------------------

- ``add_symbol(ccxt_sym)`` calls ``exchange.load_markets()`` once and
  pulls ``precision.price`` from the market metadata to register the
  symbol in an internal ``flox.SymbolRegistry`` with the right tick
  size. The strategy never has to hard-code a tick.
- ``add_strategy(strategy)`` attaches the strategy to an internal
  ``flox.Runner``. The runner's signal callback is wired to
  ``_place_order``, so emitting a signal from inside the strategy
  results in an actual ccxt order placement.
- ``run(streams=...)`` spawns one asyncio task per (symbol × stream)
  pair. ``"trades"`` and ``"book"`` forward into ``runner.on_trade`` /
  ``runner.on_book_snapshot``. ``"orders"`` calls a per-strategy
  ``on_order_update(ccxt_sym, status, filled, avg_price)`` callback if
  the strategy defines one.
- Before streams start, the broker calls ``fetch_balance()`` and
  ``fetch_positions()`` and dispatches a per-strategy
  ``on_initial_position(ccxt_sym, qty, avg_price)`` callback for any
  open position. Strategies that don't define the method are skipped.
- Stream errors are caught, reported via ``on_error``, and retried
  with exponential backoff (configurable via
  ``retry_initial_delay`` / ``retry_max_delay`` / ``retry_multiplier``).
  The delay resets to ``retry_initial_delay`` after each successful
  yield.

Order types
-----------

Mapped to ccxt's unified ``create_order`` API:

  ``MARKET``                  → ``create_market_<side>_order``
  ``LIMIT``                   → ``create_limit_<side>_order``
  ``STOP_MARKET``             → ``create_order(type='stop_market', params={'stopPrice': trigger})``
  ``STOP_LIMIT``              → ``create_order(type='stop_limit',  params={'stopPrice': trigger}, price=limit)``
  ``TAKE_PROFIT_MARKET``      → ``create_order(type='take_profit_market', params={'stopPrice': trigger})``
  ``TAKE_PROFIT_LIMIT``       → ``create_order(type='take_profit_limit',  params={'stopPrice': trigger}, price=limit)``
  ``TRAILING_STOP``           → ``create_order(type='trailing_stop_market', params={'trailingDelta': offset})``
  ``TRAILING_STOP_PERCENT``   → ``create_order(type='trailing_stop_market', params={'callbackRate': bps/100})``
  ``CLOSE_POSITION``          → ``create_order(type='market', params={'reduceOnly': True})``
  ``CANCEL``                  → ``cancel_order(ccxt_id, ccxt_sym)``
  ``CANCEL_ALL``              → ``cancel_order`` for every tracked id on this symbol
  ``MODIFY``                  → ``edit_order(...)``

Stop / take-profit / trailing semantics vary by exchange (Binance,
Bybit, OKX all use slightly different ``params`` keys). The mappings
above are the unified-API defaults. If a particular exchange needs
different params, override ``_place_order`` in a subclass.

Anything not in the table above is reported through ``on_error`` as
``UnsupportedOrderType``.
"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, Mapping, Optional

logger = logging.getLogger(__name__)


# ── Helpers ────────────────────────────────────────────────────────────


def _ccxt_ts_ns(t: Any) -> int:
    """Convert a ccxt object's millisecond timestamp to nanoseconds.

    Some exchanges emit updates without a ``timestamp`` field; fall
    back to wall clock so downstream code never sees a zero ts.
    """
    ts_ms = t.get("timestamp") if isinstance(t, dict) else None
    if ts_ms:
        return int(ts_ms) * 1_000_000
    return time.time_ns()


def _trade_is_buy(t: Mapping[str, Any]) -> bool:
    return (t.get("side") or "").lower() == "buy"


def _market_tick_size(market: Mapping[str, Any]) -> float:
    """Extract a tick size from a ccxt ``market`` dict.

    ccxt's ``precision.price`` is overloaded across exchanges: some
    expose it as the tick value (e.g. ``0.01``), others as the number
    of decimal digits (e.g. ``2``). Convention: values >= 1 are read
    as decimals. Falls back to 0.01 if neither shape parses.
    """
    p = market.get("precision") or {}
    raw = p.get("price")
    if raw is None:
        return 0.01
    try:
        v = float(raw)
    except (TypeError, ValueError):
        return 0.01
    if v <= 0:
        return 0.01
    if v >= 1:
        return 10.0 ** -int(v)
    return v


class UnsupportedOrderType(RuntimeError):
    """Raised when a strategy emits an order type the adapter can't route."""


# ── Broker ─────────────────────────────────────────────────────────────


@dataclass
class CcxtBroker:
    """Async live broker glue for FLOX strategies on top of ccxt.pro.

    Owns its own ``SymbolRegistry`` and ``Runner`` — call
    ``add_symbol`` to register symbols, ``add_strategy`` to attach
    strategies, then ``await run(...)`` to start streams.

    Parameters
    ----------
    exchange_id
        ccxt.pro exchange class name, e.g. ``"binance"`` or ``"bybit"``.
    api_key, secret, password
        Optional credentials. Leave blank for public market data only.
    sandbox
        Whether to call ``exchange.set_sandbox_mode(True)``. Not every
        exchange supports a sandbox; if not, ccxt raises and the
        broker reports it through ``on_error``.
    retry_initial_delay, retry_max_delay, retry_multiplier
        Exponential backoff parameters for stream errors. Delay starts
        at ``retry_initial_delay`` seconds, multiplies by
        ``retry_multiplier`` after each consecutive failure, caps at
        ``retry_max_delay``, and resets to the initial value after a
        successful yield.
    on_error
        Callable ``(context_str, exception) -> None`` for stream and
        order errors. ``context_str`` is the ccxt symbol or a short
        tag like ``"balance"`` / ``"orders"``. Default logs at WARN.
    exchange
        Dependency injection for tests. Production callers leave this
        ``None`` and the broker constructs the ccxt.pro exchange in
        ``_ensure_exchange``.
    """

    exchange_id: str
    api_key: Optional[str] = None
    secret: Optional[str] = None
    password: Optional[str] = None
    sandbox: bool = False

    retry_initial_delay: float = 1.0
    retry_max_delay: float = 60.0
    retry_multiplier: float = 2.0

    on_error: Optional[Callable[[str, BaseException], None]] = None
    exchange: Any = None

    _registry: Any = field(default=None, init=False, repr=False)
    _runner: Any = field(default=None, init=False, repr=False)
    _strategies: list = field(default_factory=list, init=False, repr=False)
    _sym_to_ccxt: dict = field(default_factory=dict, init=False, repr=False)
    _ccxt_to_sym: dict = field(default_factory=dict, init=False, repr=False)
    _flox_to_ccxt_order: dict = field(default_factory=dict, init=False, repr=False)
    _ccxt_orders_by_sym: dict = field(default_factory=dict, init=False, repr=False)
    _tasks: list = field(default_factory=list, init=False, repr=False)
    _loop: Optional[asyncio.AbstractEventLoop] = field(default=None, init=False, repr=False)
    _markets_loaded: bool = field(default=False, init=False, repr=False)

    def __post_init__(self) -> None:
        if self.retry_initial_delay <= 0:
            raise ValueError("retry_initial_delay must be > 0")
        if self.retry_max_delay < self.retry_initial_delay:
            raise ValueError("retry_max_delay must be >= retry_initial_delay")
        if self.retry_multiplier < 1.0:
            raise ValueError("retry_multiplier must be >= 1")
        if self.on_error is None:
            self.on_error = self._default_on_error
        import flox_py
        self._registry = flox_py.SymbolRegistry()

    @staticmethod
    def _default_on_error(context: str, exc: BaseException) -> None:
        logger.warning("ccxt broker error [%s]: %r", context, exc)

    # ── Lifecycle ─────────────────────────────────────────────────────

    async def __aenter__(self) -> "CcxtBroker":
        await self._ensure_exchange()
        return self

    async def __aexit__(self, *exc_info: Any) -> None:
        await self.close()

    async def _ensure_exchange(self) -> None:
        if self.exchange is not None:
            return
        try:
            import ccxt.pro as ccxtpro  # type: ignore
        except ImportError as e:
            raise ImportError(
                "CcxtBroker requires ccxt>=4 with the .pro async API. "
                "Install with: pip install ccxt"
            ) from e
        cls = getattr(ccxtpro, self.exchange_id, None)
        if cls is None:
            raise ValueError(f"unknown ccxt exchange {self.exchange_id!r}")
        config: dict = {}
        if self.api_key:
            config["apiKey"] = self.api_key
        if self.secret:
            config["secret"] = self.secret
        if self.password:
            config["password"] = self.password
        self.exchange = cls(config)
        if self.sandbox:
            try:
                self.exchange.set_sandbox_mode(True)
            except BaseException as e:
                if self.on_error:
                    self.on_error("sandbox", e)

    async def close(self) -> None:
        """Cancel all stream tasks, stop the runner, close the exchange."""
        await self.stop()
        if self._runner is not None:
            try:
                self._runner.stop()
            except BaseException as e:
                if self.on_error:
                    self.on_error("runner.stop", e)
            self._runner = None
        if self.exchange is not None:
            close = getattr(self.exchange, "close", None)
            if close is not None:
                try:
                    res = close()
                    if asyncio.iscoroutine(res):
                        await res
                except BaseException as e:
                    if self.on_error:
                        self.on_error("exchange.close", e)

    async def stop(self) -> None:
        """Cancel all stream tasks. Idempotent. Does not close exchange."""
        for t in self._tasks:
            t.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()

    # ── Symbol registration ──────────────────────────────────────────

    async def add_symbol(self, ccxt_sym: str) -> Any:
        """Register a ccxt symbol, return the FLOX ``Symbol`` object.

        First call triggers a one-time ``exchange.load_markets()``
        round-trip; subsequent calls reuse the cached metadata. Tick
        size comes from ``markets[ccxt_sym]["precision"]["price"]``
        via :func:`_market_tick_size`.
        """
        await self._ensure_exchange()
        if not self._markets_loaded:
            await self.exchange.load_markets()
            self._markets_loaded = True
        markets = getattr(self.exchange, "markets", None) or {}
        market = markets.get(ccxt_sym)
        if market is None:
            raise ValueError(
                f"symbol {ccxt_sym!r} is not listed on {self.exchange_id} "
                f"(after load_markets())"
            )
        tick = _market_tick_size(market)
        sym = self._registry.add_symbol(self.exchange_id, ccxt_sym, tick)
        self._sym_to_ccxt[int(sym)] = ccxt_sym
        self._ccxt_to_sym[ccxt_sym] = sym
        return sym

    def add_strategy(self, strategy: Any) -> None:
        """Queue a strategy to attach when ``run()`` starts the runner."""
        self._strategies.append(strategy)

    # ── Run loop ──────────────────────────────────────────────────────

    async def run(
        self,
        *,
        streams: Iterable[str] = ("trades", "book"),
        book_depth: int = 20,
        reconcile: bool = True,
    ) -> None:
        """Start the runner, dispatch positions, spawn streams, await.

        ``streams`` may include ``"trades"``, ``"book"``, and
        ``"orders"``. The orders stream is a single
        ``watch_orders()`` task — ccxt aggregates updates across
        symbols. Returns when every task is cancelled (typically via
        ``Ctrl+C`` or an outer ``async with`` exit).
        """
        await self._ensure_exchange()
        valid = {"trades", "book", "orders"}
        unknown = [s for s in streams if s not in valid]
        if unknown:
            raise ValueError(
                f"unknown stream(s) {unknown}; expected subset of {sorted(valid)}"
            )
        if not self._ccxt_to_sym:
            raise ValueError(
                "no symbols registered — call await broker.add_symbol(...) first"
            )

        import flox_py
        self._runner = flox_py.Runner(self._registry, on_signal=self._on_signal)
        for strat in self._strategies:
            self._runner.add_strategy(strat)
        self._runner.start()

        self._loop = asyncio.get_running_loop()

        if reconcile:
            await self._reconcile_positions()

        if "trades" in streams:
            for ccxt_sym, flox_sym in self._ccxt_to_sym.items():
                self._tasks.append(asyncio.create_task(
                    self._trades_loop(ccxt_sym, flox_sym),
                    name=f"ccxt-trades-{ccxt_sym}",
                ))
        if "book" in streams:
            for ccxt_sym, flox_sym in self._ccxt_to_sym.items():
                self._tasks.append(asyncio.create_task(
                    self._book_loop(ccxt_sym, flox_sym, book_depth),
                    name=f"ccxt-book-{ccxt_sym}",
                ))
        if "orders" in streams:
            self._tasks.append(asyncio.create_task(
                self._orders_loop(), name="ccxt-orders",
            ))

        try:
            await asyncio.gather(*self._tasks)
        except asyncio.CancelledError:
            for t in self._tasks:
                t.cancel()
            raise

    # ── Stream loops ──────────────────────────────────────────────────

    async def _stream(
        self,
        fn: Callable[..., Any],
        context: str,
        *args: Any,
        **kwargs: Any,
    ):
        """Async generator: yield results from ``fn`` with backoff retry.

        After a non-cancel exception, sleep ``delay``, multiply
        ``delay`` by ``retry_multiplier`` (capped at
        ``retry_max_delay``), retry. Reset to ``retry_initial_delay``
        after each successful yield.
        """
        delay = self.retry_initial_delay
        while True:
            try:
                result = await fn(*args, **kwargs)
            except asyncio.CancelledError:
                raise
            except BaseException as e:
                if self.on_error:
                    self.on_error(context, e)
                await asyncio.sleep(delay)
                delay = min(delay * self.retry_multiplier, self.retry_max_delay)
                continue
            delay = self.retry_initial_delay
            yield result

    async def _trades_loop(self, ccxt_sym: str, flox_sym: Any) -> None:
        sym_id = int(flox_sym)
        async for trades in self._stream(
            self.exchange.watch_trades, ccxt_sym, ccxt_sym
        ):
            for t in trades:
                self._runner.on_trade(
                    sym_id,
                    float(t["price"]),
                    float(t["amount"]),
                    _trade_is_buy(t),
                    _ccxt_ts_ns(t),
                )

    async def _book_loop(self, ccxt_sym: str, flox_sym: Any, depth: int) -> None:
        sym_id = int(flox_sym)
        async for ob in self._stream(
            self.exchange.watch_order_book, ccxt_sym, ccxt_sym, limit=depth
        ):
            bids_raw = (ob.get("bids") or [])[:depth]
            asks_raw = (ob.get("asks") or [])[:depth]
            self._runner.on_book_snapshot(
                sym_id,
                [float(b[0]) for b in bids_raw],
                [float(b[1]) for b in bids_raw],
                [float(a[0]) for a in asks_raw],
                [float(a[1]) for a in asks_raw],
                _ccxt_ts_ns(ob),
            )

    async def _orders_loop(self) -> None:
        async for orders in self._stream(self.exchange.watch_orders, "orders"):
            for o in orders:
                self._dispatch_order_update(o)

    # ── Outbound: signal → exchange order ─────────────────────────────

    def _on_signal(self, sig: Any) -> None:
        """Runner signal callback. Schedules order placement on the loop.

        Called synchronously from inside the C++ runner — sync runner
        means same asyncio thread, threaded runner means a worker
        thread. Both paths funnel into ``_handle_signal`` via
        ``run_coroutine_threadsafe`` so the ccxt call always runs on
        the broker's event loop.
        """
        try:
            sym_id = int(sig.symbol)
        except BaseException as e:
            if self.on_error:
                self.on_error("?", e)
            return
        ccxt_sym = self._sym_to_ccxt.get(sym_id)
        if ccxt_sym is None and (sig.order_type or "").upper() != "CANCEL":
            # CANCEL signals carry order_id, not necessarily a tracked
            # symbol — let _handle_signal sort it out.
            if self.on_error:
                self.on_error(
                    f"sym_id={sym_id}",
                    RuntimeError("signal for unmapped symbol"),
                )
            return
        coro = self._handle_signal(ccxt_sym, sig)
        loop = self._loop
        if loop is None:
            if self.on_error:
                self.on_error(
                    str(ccxt_sym),
                    RuntimeError("signal received before broker.run() started"),
                )
            return
        try:
            running = asyncio.get_running_loop()
        except RuntimeError:
            running = None
        if running is loop:
            loop.create_task(coro)
        else:
            asyncio.run_coroutine_threadsafe(coro, loop)

    async def _handle_signal(self, ccxt_sym: Optional[str], sig: Any) -> None:
        order_type = (getattr(sig, "order_type", "") or "").upper()
        side = (getattr(sig, "side", "") or "").lower()
        try:
            if order_type == "MARKET":
                await self._place_market(ccxt_sym, side, sig)
            elif order_type == "LIMIT":
                await self._place_limit(ccxt_sym, side, sig)
            elif order_type == "STOP_MARKET":
                await self._place_stop_market(ccxt_sym, side, sig)
            elif order_type == "STOP_LIMIT":
                await self._place_stop_limit(ccxt_sym, side, sig)
            elif order_type == "TAKE_PROFIT_MARKET":
                await self._place_tp_market(ccxt_sym, side, sig)
            elif order_type == "TAKE_PROFIT_LIMIT":
                await self._place_tp_limit(ccxt_sym, side, sig)
            elif order_type in ("TRAILING_STOP", "TRAILING_STOP_PERCENT"):
                await self._place_trailing(ccxt_sym, side, sig, order_type)
            elif order_type == "CLOSE_POSITION":
                await self._place_close(ccxt_sym, side, sig)
            elif order_type == "CANCEL":
                await self._cancel(ccxt_sym, sig)
            elif order_type == "CANCEL_ALL":
                await self._cancel_all(ccxt_sym)
            elif order_type == "MODIFY":
                await self._modify(ccxt_sym, side, sig)
            else:
                raise UnsupportedOrderType(
                    f"order_type {order_type!r} not handled by CcxtBroker"
                )
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            if self.on_error:
                self.on_error(str(ccxt_sym or "?"), e)

    # ── Order placement helpers ───────────────────────────────────────

    @staticmethod
    def _qty(sig: Any) -> float:
        q = float(getattr(sig, "quantity", 0.0))
        if q <= 0:
            raise ValueError(f"non-positive quantity {q!r} on signal")
        return q

    @staticmethod
    def _price(sig: Any) -> float:
        p = float(getattr(sig, "price", 0.0))
        if p <= 0:
            raise ValueError(f"non-positive price {p!r} on signal")
        return p

    @staticmethod
    def _trigger(sig: Any) -> float:
        p = float(getattr(sig, "trigger_price", 0.0))
        if p <= 0:
            raise ValueError(f"non-positive trigger_price {p!r} on signal")
        return p

    def _track_order(self, ccxt_sym: Optional[str], sig: Any, res: Any) -> None:
        ccxt_id = res.get("id") if isinstance(res, dict) else None
        flox_id = int(getattr(sig, "order_id", 0) or 0)
        if not ccxt_id:
            return
        if flox_id:
            self._flox_to_ccxt_order[flox_id] = str(ccxt_id)
        if ccxt_sym:
            self._ccxt_orders_by_sym.setdefault(ccxt_sym, set()).add(str(ccxt_id))

    async def _place_market(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("MARKET order requires a tracked symbol")
        qty = self._qty(sig)
        if side == "buy":
            res = await self.exchange.create_market_buy_order(sym, qty)
        elif side == "sell":
            res = await self.exchange.create_market_sell_order(sym, qty)
        else:
            raise ValueError(f"unknown side {side!r}")
        self._track_order(sym, sig, res)

    async def _place_limit(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("LIMIT order requires a tracked symbol")
        qty = self._qty(sig)
        price = self._price(sig)
        if side == "buy":
            res = await self.exchange.create_limit_buy_order(sym, qty, price)
        elif side == "sell":
            res = await self.exchange.create_limit_sell_order(sym, qty, price)
        else:
            raise ValueError(f"unknown side {side!r}")
        self._track_order(sym, sig, res)

    async def _place_stop_market(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("STOP_MARKET requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "stop_market", side, self._qty(sig), None,
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_stop_limit(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("STOP_LIMIT requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "stop_limit", side, self._qty(sig), self._price(sig),
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_tp_market(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("TAKE_PROFIT_MARKET requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "take_profit_market", side, self._qty(sig), None,
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_tp_limit(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("TAKE_PROFIT_LIMIT requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "take_profit_limit", side, self._qty(sig), self._price(sig),
            {"stopPrice": self._trigger(sig)},
        )
        self._track_order(sym, sig, res)

    async def _place_trailing(
        self, sym: Optional[str], side: str, sig: Any, kind: str,
    ) -> None:
        if not sym:
            raise ValueError(f"{kind} requires a tracked symbol")
        if kind == "TRAILING_STOP_PERCENT":
            bps = int(getattr(sig, "trailing_bps", 0) or 0)
            if bps <= 0:
                raise ValueError(f"non-positive trailing_bps {bps!r}")
            params = {"callbackRate": bps / 100.0}
        else:
            offset = float(getattr(sig, "trailing_offset", 0.0) or 0.0)
            if offset <= 0:
                raise ValueError(f"non-positive trailing_offset {offset!r}")
            params = {"trailingDelta": offset}
        res = await self.exchange.create_order(
            sym, "trailing_stop_market", side, self._qty(sig), None, params,
        )
        self._track_order(sym, sig, res)

    async def _place_close(self, sym: Optional[str], side: str, sig: Any) -> None:
        if not sym:
            raise ValueError("CLOSE_POSITION requires a tracked symbol")
        res = await self.exchange.create_order(
            sym, "market", side, self._qty(sig), None, {"reduceOnly": True},
        )
        self._track_order(sym, sig, res)

    async def _cancel(self, sym: Optional[str], sig: Any) -> None:
        flox_id = int(getattr(sig, "order_id", 0) or 0)
        ccxt_id = self._flox_to_ccxt_order.get(flox_id)
        if ccxt_id is None:
            raise ValueError(
                f"CANCEL: no ccxt order id tracked for flox order_id={flox_id}"
            )
        await self.exchange.cancel_order(ccxt_id, sym)
        self._flox_to_ccxt_order.pop(flox_id, None)
        if sym and sym in self._ccxt_orders_by_sym:
            self._ccxt_orders_by_sym[sym].discard(ccxt_id)

    async def _cancel_all(self, sym: Optional[str]) -> None:
        if not sym:
            raise ValueError("CANCEL_ALL requires a tracked symbol")
        ids = list(self._ccxt_orders_by_sym.get(sym, set()))
        if not ids:
            return
        for ccxt_id in ids:
            try:
                await self.exchange.cancel_order(ccxt_id, sym)
            except BaseException as e:
                if self.on_error:
                    self.on_error(sym, e)
        self._ccxt_orders_by_sym.pop(sym, None)
        # Drop reverse-lookup entries that pointed at this symbol's ids.
        for fid in [f for f, c in self._flox_to_ccxt_order.items() if c in ids]:
            self._flox_to_ccxt_order.pop(fid, None)

    async def _modify(self, sym: Optional[str], side: str, sig: Any) -> None:
        flox_id = int(getattr(sig, "order_id", 0) or 0)
        ccxt_id = self._flox_to_ccxt_order.get(flox_id)
        if ccxt_id is None:
            raise ValueError(
                f"MODIFY: no ccxt order id tracked for flox order_id={flox_id}"
            )
        new_price = float(getattr(sig, "new_price", 0.0) or 0.0)
        new_qty = float(getattr(sig, "new_quantity", 0.0) or 0.0)
        if new_qty <= 0:
            raise ValueError(f"MODIFY: non-positive new_quantity {new_qty!r}")
        edit = getattr(self.exchange, "edit_order", None)
        if edit is None:
            raise UnsupportedOrderType(
                f"exchange {self.exchange_id} does not support edit_order"
            )
        # ccxt uses the same `type` value the original order had — without
        # tracking it we use 'limit' as the most common modify target.
        await edit(ccxt_id, sym, "limit", side, new_qty,
                   new_price if new_price > 0 else None)

    # ── Position reconciliation ───────────────────────────────────────

    async def _reconcile_positions(self) -> None:
        """Fetch balance / positions, dispatch to strategy callbacks.

        Both calls are best-effort. ``fetch_positions`` is a derivatives
        concept — for spot-only exchanges ccxt raises ``NotSupported``,
        which we treat as "no positions" rather than fail.
        """
        try:
            await self.exchange.fetch_balance()
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            if self.on_error:
                self.on_error("balance", e)

        positions: list = []
        try:
            res = await self.exchange.fetch_positions()
            if isinstance(res, list):
                positions = res
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            msg = str(e).lower()
            if "notsupported" not in type(e).__name__.lower() and "not support" not in msg:
                if self.on_error:
                    self.on_error("positions", e)

        for pos in positions:
            ccxt_sym = pos.get("symbol", "")
            if ccxt_sym not in self._ccxt_to_sym:
                continue
            qty = float(pos.get("contracts") or pos.get("amount") or 0.0)
            avg = float(pos.get("entryPrice") or pos.get("average") or 0.0)
            if qty == 0:
                continue
            for strat in self._strategies:
                fn = getattr(strat, "on_initial_position", None)
                if callable(fn):
                    try:
                        fn(ccxt_sym, qty, avg)
                    except BaseException as e:
                        if self.on_error:
                            self.on_error(ccxt_sym, e)

    # ── Order updates ─────────────────────────────────────────────────

    def _dispatch_order_update(self, o: Mapping[str, Any]) -> None:
        ccxt_sym = str(o.get("symbol", ""))
        status = str(o.get("status", ""))
        filled = float(o.get("filled") or 0.0)
        avg = float(o.get("average") or o.get("price") or 0.0)
        for strat in self._strategies:
            fn = getattr(strat, "on_order_update", None)
            if callable(fn):
                try:
                    fn(ccxt_sym, status, filled, avg)
                except BaseException as e:
                    if self.on_error:
                        self.on_error(ccxt_sym, e)


__all__ = ["CcxtBroker", "UnsupportedOrderType"]
