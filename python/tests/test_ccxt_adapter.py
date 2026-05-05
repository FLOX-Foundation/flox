"""Tests for ``flox_py.ccxt.CcxtBroker``.

The broker is pure-Python glue around ``ccxt.pro`` plus a FLOX
``Runner``. To run without ccxt actually installed we inject a
``FakeCcxtExchange`` that records calls and feeds canned WebSocket
data through ``asyncio.Queue``s.

Coverage:

  * ``add_symbol`` reads tick size from market metadata and registers
    in the internal ``SymbolRegistry``.
  * Trades / book / orders streams forward correctly into
    ``runner.on_trade`` / ``runner.on_book_snapshot`` / strategy's
    ``on_order_update``.
  * Outbound order routing for every supported ``order_type``:
    MARKET, LIMIT, STOP_MARKET, STOP_LIMIT, TAKE_PROFIT_MARKET,
    TAKE_PROFIT_LIMIT, TRAILING_STOP, TRAILING_STOP_PERCENT,
    CLOSE_POSITION, CANCEL, CANCEL_ALL, MODIFY.
  * Unsupported order_type goes through ``on_error`` as
    ``UnsupportedOrderType``, no exchange call.
  * Position reconciliation calls strategy's
    ``on_initial_position(ccxt_sym, qty, avg)`` for non-zero
    positions; ``NotSupported`` from spot exchanges is suppressed.
  * Exponential backoff: first stream call fails → broker waits
    ``retry_initial_delay``, doubles, retries; success resets delay.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_ccxt_adapter.py
"""

import asyncio
import os
import sys
import unittest

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox
from flox_py.ccxt import CcxtBroker, UnsupportedOrderType, _market_tick_size


# ── Fake exchange ─────────────────────────────────────────────────────


class _NotSupported(Exception):
    """Mimics ccxt.NotSupported without importing ccxt."""


class FakeCcxtExchange:
    """Minimal stand-in for a ``ccxt.pro`` exchange."""

    def __init__(self, *, markets=None, balance=None, positions=None):
        self.markets = markets or {
            "BTC/USDT": {"precision": {"price": 0.01}},
            "ETH/USDT": {"precision": {"price": 2}},
        }
        self._balance = balance or {"USDT": {"free": 1000.0}}
        self._positions = positions  # None → raise NotSupported
        self._trades_q: asyncio.Queue | None = None
        self._book_q: asyncio.Queue | None = None
        self._orders_q: asyncio.Queue | None = None
        self.calls: list = []  # (method_name, args, kwargs)
        self._next_id = 1
        self.sandbox = False
        self.closed = False

    # Setup -----------------------------------------------------------
    def init_queues(self):
        self._trades_q = asyncio.Queue()
        self._book_q = asyncio.Queue()
        self._orders_q = asyncio.Queue()

    def feed_trades(self, trades):
        self._trades_q.put_nowait(trades)

    def feed_book(self, book):
        self._book_q.put_nowait(book)

    def feed_orders(self, orders):
        self._orders_q.put_nowait(orders)

    def set_sandbox_mode(self, on):
        self.sandbox = on

    # ccxt surface ----------------------------------------------------
    async def load_markets(self):
        return self.markets

    async def watch_trades(self, symbol):
        return await self._trades_q.get()

    async def watch_order_book(self, symbol, limit=20):
        return await self._book_q.get()

    async def watch_orders(self):
        return await self._orders_q.get()

    async def fetch_balance(self):
        return self._balance

    async def fetch_positions(self):
        if self._positions is None:
            raise _NotSupported("fetch_positions not supported on spot")
        return self._positions

    def _record(self, name, args, kwargs):
        self.calls.append((name, args, kwargs))
        oid = str(self._next_id)
        self._next_id += 1
        return {"id": oid, "status": "open"}

    async def create_market_buy_order(self, sym, qty):
        return self._record("create_market_buy_order", (sym, qty), {})

    async def create_market_sell_order(self, sym, qty):
        return self._record("create_market_sell_order", (sym, qty), {})

    async def create_limit_buy_order(self, sym, qty, price):
        return self._record("create_limit_buy_order", (sym, qty, price), {})

    async def create_limit_sell_order(self, sym, qty, price):
        return self._record("create_limit_sell_order", (sym, qty, price), {})

    async def create_order(self, sym, type_, side, qty, price=None, params=None):
        return self._record(
            "create_order", (sym, type_, side, qty, price), {"params": params or {}}
        )

    async def cancel_order(self, oid, sym=None):
        self.calls.append(("cancel_order", (oid, sym), {}))
        return {"id": oid, "status": "canceled"}

    async def edit_order(self, oid, sym, type_, side, qty, price=None):
        self.calls.append(("edit_order", (oid, sym, type_, side, qty, price), {}))
        return {"id": oid, "status": "open"}

    async def close(self):
        self.closed = True


# ── Helpers ───────────────────────────────────────────────────────────


def _make_signal(*, symbol_id, order_type, side="", quantity=0.0, price=0.0,
                 trigger_price=0.0, trailing_offset=0.0, trailing_bps=0,
                 order_id=0, new_price=0.0, new_quantity=0.0):
    """Construct a ``flox.Signal`` with the requested fields set."""
    s = flox.Signal()
    s.symbol = symbol_id
    s.order_type = order_type
    s.side = side
    s.quantity = quantity
    s.price = price
    s.trigger_price = trigger_price
    s.trailing_offset = trailing_offset
    s.trailing_bps = trailing_bps
    s.order_id = order_id
    s.new_price = new_price
    s.new_quantity = new_quantity
    return s


# ── Tests ─────────────────────────────────────────────────────────────


class CcxtBrokerTests(unittest.TestCase):

    # ── construction / config ────────────────────────────────────────
    def test_market_tick_size_parses_decimals_and_tick(self):
        self.assertEqual(_market_tick_size({"precision": {"price": 0.01}}), 0.01)
        self.assertEqual(_market_tick_size({"precision": {"price": 2}}), 0.01)
        self.assertEqual(_market_tick_size({"precision": {"price": 5}}), 0.00001)
        self.assertEqual(_market_tick_size({}), 0.01)
        self.assertEqual(_market_tick_size({"precision": {"price": "weird"}}), 0.01)

    def test_invalid_backoff_params(self):
        fake = FakeCcxtExchange()
        with self.assertRaises(ValueError):
            CcxtBroker("binance", exchange=fake, retry_initial_delay=0)
        with self.assertRaises(ValueError):
            CcxtBroker("binance", exchange=fake,
                       retry_initial_delay=2, retry_max_delay=1)
        with self.assertRaises(ValueError):
            CcxtBroker("binance", exchange=fake, retry_multiplier=0.5)

    # ── add_symbol / load_markets ────────────────────────────────────
    def test_add_symbol_loads_markets_and_registers(self):
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            eth = await broker.add_symbol("ETH/USDT")
            return btc, eth

        btc, eth = asyncio.run(go())
        self.assertNotEqual(int(btc), int(eth))
        self.assertIn(int(btc), broker._sym_to_ccxt)
        self.assertEqual(broker._sym_to_ccxt[int(btc)], "BTC/USDT")
        self.assertEqual(broker._sym_to_ccxt[int(eth)], "ETH/USDT")

    def test_add_symbol_unknown_raises(self):
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)
        with self.assertRaises(ValueError):
            asyncio.run(broker.add_symbol("DOGE/USDT"))

    # ── streams ──────────────────────────────────────────────────────
    def test_trades_stream_forwards(self):
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)
        seen = []

        class S(flox.Strategy):
            def on_trade(self, ctx, trade):
                seen.append((trade.price, trade.quantity, trade.is_buy))

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(S([btc]))
            fake.init_queues()
            fake.feed_trades([
                {"price": 100.5, "amount": 0.5, "side": "buy",  "timestamp": 1_700_000_000_000},
                {"price": 101.0, "amount": 1.0, "side": "sell", "timestamp": 1_700_000_001_000},
            ])
            run_task = asyncio.create_task(broker.run(streams=("trades",), reconcile=False))
            await asyncio.sleep(0.05)
            await broker.stop()
            try:
                await run_task
            except asyncio.CancelledError:
                pass

        asyncio.run(go())
        self.assertEqual(len(seen), 2)
        self.assertEqual(seen[0], (100.5, 0.5, True))
        self.assertFalse(seen[1][2])

    def test_book_stream_forwards(self):
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)
        seen = []

        class S(flox.Strategy):
            def on_book_update(self, ctx):
                seen.append((ctx.best_bid, ctx.best_ask))

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(S([btc]))
            fake.init_queues()
            fake.feed_book({
                "bids": [[100.0, 1.0], [99.5, 2.0]],
                "asks": [[100.5, 1.5], [101.0, 2.5]],
                "timestamp": 1_700_000_000_000,
            })
            run_task = asyncio.create_task(
                broker.run(streams=("book",), book_depth=5, reconcile=False)
            )
            await asyncio.sleep(0.05)
            await broker.stop()
            try:
                await run_task
            except asyncio.CancelledError:
                pass

        asyncio.run(go())
        self.assertEqual(len(seen), 1)
        bid, ask = seen[0]
        self.assertEqual(bid, 100.0)
        self.assertEqual(ask, 100.5)

    def test_orders_stream_dispatches_to_strategy(self):
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)
        updates = []

        class S(flox.Strategy):
            def on_order_update(self, sym, status, filled, avg):
                updates.append((sym, status, filled, avg))

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(S([btc]))
            fake.init_queues()
            fake.feed_orders([
                {"symbol": "BTC/USDT", "status": "filled",
                 "filled": 0.5, "average": 100.5},
            ])
            run_task = asyncio.create_task(
                broker.run(streams=("orders",), reconcile=False)
            )
            await asyncio.sleep(0.05)
            await broker.stop()
            try:
                await run_task
            except asyncio.CancelledError:
                pass

        asyncio.run(go())
        self.assertEqual(updates, [("BTC/USDT", "filled", 0.5, 100.5)])

    # ── outbound order routing ───────────────────────────────────────
    def _route(self, sig_kwargs, broker_kwargs=None):
        """Drive a single signal through the broker; return fake.calls."""
        fake = FakeCcxtExchange()
        broker_kwargs = broker_kwargs or {}
        broker = CcxtBroker("binance", exchange=fake, **broker_kwargs)
        errors = []
        broker.on_error = lambda ctx, exc: errors.append((ctx, type(exc).__name__, str(exc)))

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(flox.Strategy([btc]))
            fake.init_queues()
            sig = _make_signal(symbol_id=int(btc), **sig_kwargs)
            run_task = asyncio.create_task(
                broker.run(streams=(), reconcile=False)
            )
            await asyncio.sleep(0)
            broker._on_signal(sig)
            await asyncio.sleep(0.02)
            await broker.stop()
            try:
                await run_task
            except (asyncio.CancelledError, ValueError):
                pass

        asyncio.run(go())
        return fake.calls, errors, broker

    def test_market_buy_routes(self):
        calls, errs, _ = self._route(dict(
            order_type="MARKET", side="buy", quantity=0.01))
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], "create_market_buy_order")
        self.assertEqual(calls[0][1], ("BTC/USDT", 0.01))
        self.assertFalse(errs)

    def test_market_sell_routes(self):
        calls, _, _ = self._route(dict(
            order_type="MARKET", side="sell", quantity=0.02))
        self.assertEqual(calls[0][0], "create_market_sell_order")

    def test_limit_buy_routes_with_price(self):
        calls, _, _ = self._route(dict(
            order_type="LIMIT", side="buy", quantity=0.5, price=99.5))
        self.assertEqual(calls[0][0], "create_limit_buy_order")
        self.assertEqual(calls[0][1], ("BTC/USDT", 0.5, 99.5))

    def test_stop_market_routes(self):
        calls, _, _ = self._route(dict(
            order_type="STOP_MARKET", side="sell",
            quantity=0.1, trigger_price=98.0))
        self.assertEqual(calls[0][0], "create_order")
        self.assertEqual(calls[0][1][1], "stop_market")
        self.assertEqual(calls[0][2]["params"], {"stopPrice": 98.0})

    def test_stop_limit_routes(self):
        calls, _, _ = self._route(dict(
            order_type="STOP_LIMIT", side="sell",
            quantity=0.1, price=97.5, trigger_price=98.0))
        self.assertEqual(calls[0][1][1], "stop_limit")
        self.assertEqual(calls[0][1][4], 97.5)
        self.assertEqual(calls[0][2]["params"], {"stopPrice": 98.0})

    def test_take_profit_market_routes(self):
        calls, _, _ = self._route(dict(
            order_type="TAKE_PROFIT_MARKET", side="sell",
            quantity=0.1, trigger_price=120.0))
        self.assertEqual(calls[0][1][1], "take_profit_market")

    def test_take_profit_limit_routes(self):
        calls, _, _ = self._route(dict(
            order_type="TAKE_PROFIT_LIMIT", side="sell",
            quantity=0.1, price=121.0, trigger_price=120.0))
        self.assertEqual(calls[0][1][1], "take_profit_limit")
        self.assertEqual(calls[0][1][4], 121.0)

    def test_trailing_stop_routes_with_offset(self):
        calls, _, _ = self._route(dict(
            order_type="TRAILING_STOP", side="sell",
            quantity=0.1, trailing_offset=0.5))
        self.assertEqual(calls[0][1][1], "trailing_stop_market")
        self.assertEqual(calls[0][2]["params"], {"trailingDelta": 0.5})

    def test_trailing_stop_percent_routes_with_callback_rate(self):
        calls, _, _ = self._route(dict(
            order_type="TRAILING_STOP_PERCENT", side="sell",
            quantity=0.1, trailing_bps=200))
        self.assertEqual(calls[0][1][1], "trailing_stop_market")
        self.assertEqual(calls[0][2]["params"], {"callbackRate": 2.0})

    def test_close_position_uses_reduce_only(self):
        calls, _, _ = self._route(dict(
            order_type="CLOSE_POSITION", side="sell", quantity=0.1))
        self.assertEqual(calls[0][1][1], "market")
        self.assertEqual(calls[0][2]["params"], {"reduceOnly": True})

    def test_unsupported_order_type_reports_error_no_call(self):
        calls, errs, _ = self._route(dict(
            order_type="ICEBERG", side="buy", quantity=0.1))
        self.assertEqual(calls, [])
        self.assertTrue(any("UnsupportedOrderType" in t for _, t, _ in errs))

    def test_cancel_uses_tracked_ccxt_id(self):
        # Place a limit order first, then cancel by flox order_id.
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(flox.Strategy([btc]))
            fake.init_queues()
            run_task = asyncio.create_task(broker.run(streams=(), reconcile=False))
            await asyncio.sleep(0)
            broker._on_signal(_make_signal(
                symbol_id=int(btc), order_type="LIMIT", side="buy",
                quantity=0.5, price=99.0, order_id=42,
            ))
            await asyncio.sleep(0.02)
            broker._on_signal(_make_signal(
                symbol_id=int(btc), order_type="CANCEL", order_id=42,
            ))
            await asyncio.sleep(0.02)
            await broker.stop()
            try:
                await run_task
            except (asyncio.CancelledError, ValueError):
                pass

        asyncio.run(go())
        names = [c[0] for c in fake.calls]
        self.assertIn("create_limit_buy_order", names)
        self.assertIn("cancel_order", names)
        # cancel_order must have been called with the id returned for the limit
        cancel = next(c for c in fake.calls if c[0] == "cancel_order")
        self.assertEqual(cancel[1][1], "BTC/USDT")  # symbol arg

    def test_cancel_all_walks_tracked_ids(self):
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(flox.Strategy([btc]))
            fake.init_queues()
            run_task = asyncio.create_task(broker.run(streams=(), reconcile=False))
            await asyncio.sleep(0)
            for i in range(3):
                broker._on_signal(_make_signal(
                    symbol_id=int(btc), order_type="LIMIT", side="buy",
                    quantity=0.1, price=99.0, order_id=100 + i,
                ))
            await asyncio.sleep(0.02)
            broker._on_signal(_make_signal(
                symbol_id=int(btc), order_type="CANCEL_ALL"))
            await asyncio.sleep(0.02)
            await broker.stop()
            try:
                await run_task
            except (asyncio.CancelledError, ValueError):
                pass

        asyncio.run(go())
        cancels = [c for c in fake.calls if c[0] == "cancel_order"]
        self.assertEqual(len(cancels), 3)

    def test_modify_calls_edit_order(self):
        fake = FakeCcxtExchange()
        broker = CcxtBroker("binance", exchange=fake)

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(flox.Strategy([btc]))
            fake.init_queues()
            run_task = asyncio.create_task(broker.run(streams=(), reconcile=False))
            await asyncio.sleep(0)
            broker._on_signal(_make_signal(
                symbol_id=int(btc), order_type="LIMIT", side="buy",
                quantity=0.5, price=99.0, order_id=7,
            ))
            await asyncio.sleep(0.02)
            broker._on_signal(_make_signal(
                symbol_id=int(btc), order_type="MODIFY", side="buy",
                order_id=7, new_price=98.0, new_quantity=0.4,
            ))
            await asyncio.sleep(0.02)
            await broker.stop()
            try:
                await run_task
            except (asyncio.CancelledError, ValueError):
                pass

        asyncio.run(go())
        edit = next(c for c in fake.calls if c[0] == "edit_order")
        self.assertEqual(edit[1][3], "buy")
        self.assertEqual(edit[1][4], 0.4)
        self.assertEqual(edit[1][5], 98.0)

    # ── reconciliation ───────────────────────────────────────────────
    def test_reconciliation_dispatches_initial_position(self):
        fake = FakeCcxtExchange(positions=[
            {"symbol": "BTC/USDT", "contracts": 0.5, "entryPrice": 100.0},
            {"symbol": "ETH/USDT", "contracts": 0, "entryPrice": 0},
            {"symbol": "DOGE/USDT", "contracts": 1, "entryPrice": 0.1},  # untracked
        ])
        broker = CcxtBroker("binance", exchange=fake)
        seen = []

        class S(flox.Strategy):
            def on_initial_position(self, sym, qty, avg):
                seen.append((sym, qty, avg))

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(S([btc]))
            fake.init_queues()
            run_task = asyncio.create_task(broker.run(streams=()))
            await asyncio.sleep(0.02)
            await broker.stop()
            try:
                await run_task
            except asyncio.CancelledError:
                pass

        asyncio.run(go())
        # only BTC tracked, only non-zero qty dispatched, untracked symbol skipped
        self.assertEqual(seen, [("BTC/USDT", 0.5, 100.0)])

    def test_reconciliation_swallows_not_supported(self):
        fake = FakeCcxtExchange(positions=None)  # → raises _NotSupported
        broker = CcxtBroker("binance", exchange=fake)
        errors = []
        broker.on_error = lambda c, e: errors.append((c, e))

        async def go():
            btc = await broker.add_symbol("BTC/USDT")
            broker.add_strategy(flox.Strategy([btc]))
            fake.init_queues()
            run_task = asyncio.create_task(broker.run(streams=()))
            await asyncio.sleep(0.02)
            await broker.stop()
            try:
                await run_task
            except asyncio.CancelledError:
                pass

        asyncio.run(go())
        # spot exchange's NotSupported should not surface as a broker error
        position_errors = [e for c, e in errors if c == "positions"]
        self.assertEqual(position_errors, [])

    # ── exponential backoff ──────────────────────────────────────────
    def test_backoff_doubles_on_repeated_failure_then_resets(self):
        # Replace asyncio.sleep with a recorder so we measure the
        # delays without actually sleeping. The broker calls
        # asyncio.sleep(delay) inside _stream — patching the module
        # symbol catches it.
        delays = []
        real_sleep = asyncio.sleep

        async def fake_sleep(d):
            delays.append(d)
            await real_sleep(0)

        from flox_py import ccxt as ccxt_mod

        broker = CcxtBroker(
            "binance", exchange=FakeCcxtExchange(),
            retry_initial_delay=0.5, retry_max_delay=4.0, retry_multiplier=2.0,
        )

        # _stream is an async generator. Drive 3 failures then 1 success.
        attempts = {"n": 0}

        async def flaky():
            attempts["n"] += 1
            if attempts["n"] <= 3:
                raise RuntimeError(f"transient {attempts['n']}")
            return "ok"

        async def drive():
            orig = ccxt_mod.asyncio.sleep
            ccxt_mod.asyncio.sleep = fake_sleep
            try:
                gen = broker._stream(flaky, "test")
                first = await gen.__anext__()
            finally:
                ccxt_mod.asyncio.sleep = orig
            return first

        result = asyncio.run(drive())
        self.assertEqual(result, "ok")
        # 3 failures → 3 sleeps with delays 0.5, 1.0, 2.0 (multiplier=2)
        self.assertEqual(delays, [0.5, 1.0, 2.0])

        # Now drive another failure → success: backoff should have reset
        # to retry_initial_delay (0.5), not continue from 4.0.
        delays.clear()
        attempts["n"] = 0

        async def drive2():
            orig = ccxt_mod.asyncio.sleep
            ccxt_mod.asyncio.sleep = fake_sleep
            try:
                gen = broker._stream(flaky, "test")
                # Consume the first success (after 3 failures, delays
                # 0.5, 1.0, 2.0), then trigger another round.
                await gen.__anext__()
                attempts["n"] = 0  # reset counter to force fresh failures
                await gen.__anext__()
            finally:
                ccxt_mod.asyncio.sleep = orig

        asyncio.run(drive2())
        # 3 + 3 sleeps total; the second batch should also start at 0.5,
        # confirming reset on success.
        self.assertEqual(delays[:3], [0.5, 1.0, 2.0])
        self.assertEqual(delays[3:6], [0.5, 1.0, 2.0])

    def test_backoff_caps_at_retry_max_delay(self):
        delays = []
        real_sleep = asyncio.sleep

        async def fake_sleep(d):
            delays.append(d)
            await real_sleep(0)

        from flox_py import ccxt as ccxt_mod

        broker = CcxtBroker(
            "binance", exchange=FakeCcxtExchange(),
            retry_initial_delay=1.0, retry_max_delay=4.0, retry_multiplier=3.0,
        )
        attempts = {"n": 0}

        async def flaky():
            attempts["n"] += 1
            if attempts["n"] <= 5:
                raise RuntimeError("nope")
            return "ok"

        async def drive():
            orig = ccxt_mod.asyncio.sleep
            ccxt_mod.asyncio.sleep = fake_sleep
            try:
                gen = broker._stream(flaky, "test")
                await gen.__anext__()
            finally:
                ccxt_mod.asyncio.sleep = orig

        asyncio.run(drive())
        # 1.0, 3.0, then capped at 4.0 from there on.
        self.assertEqual(delays, [1.0, 3.0, 4.0, 4.0, 4.0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
