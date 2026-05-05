"""
python/tests/test_ccxt_adapter.py — verify CcxtFeed forwards trades and
book updates from a mock ccxt exchange into a flox.Runner.

Doesn't require ccxt to be installed; we mock the parts we use
(watch_trades / watch_order_book / close).

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_ccxt_adapter.py
"""

import asyncio
import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox
from flox_py.ccxt import CcxtFeed

_passed = 0
_failed = 0


def check(cond, msg):
    global _passed, _failed
    if cond:
        print(f"  ok  {msg}")
        _passed += 1
    else:
        print(f"  FAIL  {msg}")
        _failed += 1


# ── Mock ccxt exchange ────────────────────────────────────────────────


class FakeCcxtExchange:
    """Minimal stand-in for a ccxt.pro exchange.

    `_trades` and `_books` are queues fed from the test; the watch_*
    methods pop from them (or wait forever if empty), simulating WS
    streams. Realistic enough for a smoke test.
    """

    def __init__(self):
        self._trades_q: asyncio.Queue = None
        self._book_q: asyncio.Queue = None

    def init_queues(self):
        self._trades_q = asyncio.Queue()
        self._book_q = asyncio.Queue()

    def feed_trades(self, trades):
        self._trades_q.put_nowait(trades)

    def feed_book(self, book):
        self._book_q.put_nowait(book)

    async def watch_trades(self, symbol):
        return await self._trades_q.get()

    async def watch_order_book(self, symbol, limit=20):
        return await self._book_q.get()


# ── Tests ─────────────────────────────────────────────────────────────


def test_trades_forwarded():
    print("test_trades_forwarded")
    reg = flox.SymbolRegistry()
    btc = reg.add_symbol("test", "BTCUSDT", 0.01)
    seen_trades = []

    class S(flox.Strategy):
        def on_trade(self, ctx, trade):
            seen_trades.append((trade.price, trade.quantity, trade.is_buy))

    runner = flox.Runner(reg, lambda sig: None, threaded=False)
    runner.add_strategy(S([btc]))
    runner.start()

    fake = FakeCcxtExchange()
    feed = CcxtFeed(
        exchange=fake,
        runner=runner,
        symbols={"BTC/USDT": btc},
        streams=("trades",),
    )

    async def driver():
        fake.init_queues()
        # Push a couple of trades.
        fake.feed_trades([
            {"price": 100.5, "amount": 0.5, "side": "buy",  "timestamp": 1_700_000_000_000},
            {"price": 101.0, "amount": 1.0, "side": "sell", "timestamp": 1_700_000_001_000},
        ])
        # Run feed for a tiny window then cancel.
        run_task = asyncio.create_task(feed.run())
        await asyncio.sleep(0.05)
        await feed.stop()
        try:
            await run_task
        except asyncio.CancelledError:
            pass

    asyncio.run(driver())
    runner.stop()

    check(len(seen_trades) == 2, f"two trades forwarded (got {len(seen_trades)})")
    if seen_trades:
        check(seen_trades[0] == (100.5, 0.5, True),
              f"first trade is buy 100.5 / 0.5 (got {seen_trades[0]})")
        check(seen_trades[1][2] is False,
              f"second trade is sell (got is_buy={seen_trades[1][2]})")


def test_book_forwarded():
    print("test_book_forwarded")
    reg = flox.SymbolRegistry()
    btc = reg.add_symbol("test", "BTCUSDT", 0.01)
    seen_books = []

    class S(flox.Strategy):
        def on_book_update(self, ctx):
            seen_books.append((ctx.best_bid, ctx.best_ask))

    runner = flox.Runner(reg, lambda sig: None, threaded=False)
    runner.add_strategy(S([btc]))
    runner.start()

    fake = FakeCcxtExchange()
    feed = CcxtFeed(
        exchange=fake,
        runner=runner,
        symbols={"BTC/USDT": btc},
        streams=("book",),
        book_depth=5,
    )

    async def driver():
        fake.init_queues()
        fake.feed_book({
            "bids": [[100.0, 1.0], [99.5, 2.0]],
            "asks": [[100.5, 1.5], [101.0, 2.5]],
            "timestamp": 1_700_000_000_000,
        })
        run_task = asyncio.create_task(feed.run())
        await asyncio.sleep(0.05)
        await feed.stop()
        try:
            await run_task
        except asyncio.CancelledError:
            pass

    asyncio.run(driver())
    runner.stop()

    check(len(seen_books) == 1, f"one book update forwarded (got {len(seen_books)})")
    if seen_books:
        bid, ask = seen_books[0]
        check(bid == 100.0, f"best bid 100.0 (got {bid})")
        check(ask == 100.5, f"best ask 100.5 (got {ask})")


def test_validation_rejects_unknown_stream():
    print("test_validation_rejects_unknown_stream")
    reg = flox.SymbolRegistry()
    btc = reg.add_symbol("test", "BTCUSDT", 0.01)
    runner = flox.Runner(reg, lambda sig: None, threaded=False)
    try:
        CcxtFeed(
            exchange=FakeCcxtExchange(),
            runner=runner,
            symbols={"BTC/USDT": btc},
            streams=("trades", "ticker"),  # ticker is not supported
        )
        check(False, "expected ValueError for unknown stream")
    except ValueError as e:
        check("ticker" in str(e), "ValueError mentions the bad stream name")


def test_validation_rejects_empty_symbols():
    print("test_validation_rejects_empty_symbols")
    reg = flox.SymbolRegistry()
    runner = flox.Runner(reg, lambda sig: None, threaded=False)
    try:
        CcxtFeed(
            exchange=FakeCcxtExchange(),
            runner=runner,
            symbols={},
            streams=("trades",),
        )
        check(False, "expected ValueError for empty symbols")
    except ValueError as e:
        check("must not be empty" in str(e),
              f"ValueError mentions empty symbols (got {e})")


if __name__ == "__main__":
    test_trades_forwarded()
    test_book_forwarded()
    test_validation_rejects_unknown_stream()
    test_validation_rejects_empty_symbols()
    print(f"\n{_passed} passed, {_failed} failed")
    sys.exit(0 if _failed == 0 else 1)
