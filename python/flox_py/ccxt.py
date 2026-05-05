"""CCXT live data adapter for FLOX.

Wraps a ``ccxt.pro`` exchange and forwards trades / book updates into a
``flox_py.Runner``. Lets you connect any of CCXT's 100+ supported
exchanges to FLOX without writing the WebSocket plumbing yourself.

Quick start
-----------

.. code-block:: python

    import asyncio
    import ccxt.pro as ccxtpro
    import flox_py as flox
    from flox_py.ccxt import CcxtFeed

    registry = flox.SymbolRegistry()
    btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    runner = flox.Runner(registry, on_signal=lambda s: print(s))
    runner.add_strategy(MyStrategy([btc]))
    runner.start()

    exchange = ccxtpro.binance()
    feed = CcxtFeed(
        exchange=exchange,
        runner=runner,
        symbols={"BTC/USDT": btc},
        streams=("trades", "book"),
    )

    try:
        asyncio.run(feed.run())
    finally:
        runner.stop()
        asyncio.run(exchange.close())

Order routing
-------------

CcxtFeed only handles **inbound** market data. Order placement is up to
your strategy's signal handler:

.. code-block:: python

    def on_signal(sig):
        if sig.side == "buy":
            asyncio.create_task(
                exchange.create_market_buy_order("BTC/USDT", sig.quantity)
            )

This split keeps the inbound (FLOX-controlled) and outbound (broker-
controlled) paths cleanly separable.
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
    """Extract a nanosecond timestamp from a ccxt trade / book object.

    ccxt timestamps are in milliseconds; we convert to ns. Falls back to
    ``time.time_ns()`` if the field is missing or zero (some exchanges
    occasionally emit updates without a ts).
    """
    ts_ms = t.get("timestamp") if isinstance(t, dict) else None
    if ts_ms:
        return int(ts_ms) * 1_000_000
    return time.time_ns()


def _trade_is_buy(t: Mapping[str, Any]) -> bool:
    """Map a ccxt trade dict's ``side`` to a buy/sell boolean."""
    side = (t.get("side") or "").lower()
    return side == "buy"


# ── Feed ───────────────────────────────────────────────────────────────


@dataclass
class CcxtFeed:
    """Bridges a ``ccxt.pro`` exchange into a ``flox_py.Runner``.

    Parameters
    ----------
    exchange
        A ``ccxt.pro`` exchange instance, already configured (api keys
        if you intend to trade — not needed for public market data).
    runner
        The FLOX ``Runner`` to forward events into. Must be started
        *before* ``run()`` is awaited so its strategy callbacks are live.
    symbols
        Mapping ``{ccxt_symbol: flox_symbol}``. The CCXT symbol is the
        format the exchange expects (e.g. ``"BTC/USDT"`` or
        ``"BTC/USDT:USDT"`` for perpetuals); the FLOX symbol is whatever
        ``SymbolRegistry.add_symbol()`` returned (or its ``.id``).
    streams
        Which streams to subscribe to. ``"trades"`` and/or ``"book"``.
        Defaults to both.
    book_depth
        L2 depth (number of bid/ask levels) to forward. Most exchanges
        publish 20 levels by default; set lower if your strategy doesn't
        need full depth.
    on_error
        Optional callable ``(symbol_str, exception) -> None`` invoked
        when a stream raises. Defaults to logging via the standard
        ``logging`` module. Set to ``None`` to silence.
    """

    exchange: Any
    runner: Any
    symbols: Mapping[str, Any]
    streams: Iterable[str] = ("trades", "book")
    book_depth: int = 20
    on_error: Optional[Callable[[str, BaseException], None]] = None

    _tasks: list[asyncio.Task] = field(default_factory=list, init=False, repr=False)

    def __post_init__(self) -> None:
        # Validate streams up front so the user gets a TypeError now
        # rather than mid-loop.
        valid = {"trades", "book"}
        for s in self.streams:
            if s not in valid:
                raise ValueError(
                    f"unknown stream {s!r}; expected one of {sorted(valid)}"
                )
        if not self.symbols:
            raise ValueError("CcxtFeed: symbols mapping must not be empty")
        if self.on_error is None:
            self.on_error = self._default_on_error

    @staticmethod
    def _default_on_error(symbol: str, exc: BaseException) -> None:
        logger.warning("ccxt feed error on %s: %r", symbol, exc)

    # ── Public API ────────────────────────────────────────────────────

    async def run(self) -> None:
        """Run all configured streams concurrently. Cancellation-safe.

        Returns when every stream task has been cancelled. Typically
        you'd call this with ``asyncio.run(feed.run())`` and let
        Ctrl+C / signal cancellation tear it down; the calling code is
        responsible for closing the ccxt exchange afterwards.
        """
        try:
            for ccxt_sym, flox_sym in self.symbols.items():
                if "trades" in self.streams:
                    self._tasks.append(
                        asyncio.create_task(self._trades_loop(ccxt_sym, flox_sym),
                                             name=f"ccxt-trades-{ccxt_sym}")
                    )
                if "book" in self.streams:
                    self._tasks.append(
                        asyncio.create_task(self._book_loop(ccxt_sym, flox_sym),
                                             name=f"ccxt-book-{ccxt_sym}")
                    )
            await asyncio.gather(*self._tasks)
        except asyncio.CancelledError:
            for t in self._tasks:
                t.cancel()
            raise

    async def stop(self) -> None:
        """Cancel all running stream tasks. Idempotent."""
        for t in self._tasks:
            t.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()

    # ── Stream loops ──────────────────────────────────────────────────

    async def _trades_loop(self, ccxt_sym: str, flox_sym: Any) -> None:
        sym_id = int(flox_sym)
        while True:
            try:
                trades = await self.exchange.watch_trades(ccxt_sym)
            except asyncio.CancelledError:
                raise
            except BaseException as e:
                if self.on_error:
                    self.on_error(ccxt_sym, e)
                # Brief backoff to avoid busy-looping on persistent errors.
                await asyncio.sleep(1.0)
                continue
            for t in trades:
                self.runner.on_trade(
                    sym_id,
                    float(t["price"]),
                    float(t["amount"]),
                    _trade_is_buy(t),
                    _ccxt_ts_ns(t),
                )

    async def _book_loop(self, ccxt_sym: str, flox_sym: Any) -> None:
        sym_id = int(flox_sym)
        while True:
            try:
                ob = await self.exchange.watch_order_book(ccxt_sym, limit=self.book_depth)
            except asyncio.CancelledError:
                raise
            except BaseException as e:
                if self.on_error:
                    self.on_error(ccxt_sym, e)
                await asyncio.sleep(1.0)
                continue
            bids_raw = ob.get("bids", [])[: self.book_depth]
            asks_raw = ob.get("asks", [])[: self.book_depth]
            bid_prices = [float(b[0]) for b in bids_raw]
            bid_qtys = [float(b[1]) for b in bids_raw]
            ask_prices = [float(a[0]) for a in asks_raw]
            ask_qtys = [float(a[1]) for a in asks_raw]
            self.runner.on_book_snapshot(
                sym_id,
                bid_prices, bid_qtys,
                ask_prices, ask_qtys,
                _ccxt_ts_ns(ob),
            )


__all__ = ["CcxtFeed"]
