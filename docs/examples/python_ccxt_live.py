"""
Live SMA crossover strategy fed from Binance via ccxt.pro WebSocket.

Connects to Binance public WebSocket, streams BTC/USDT trades in real time,
runs SMA(10/30) crossover strategy, prints signals.

No API key needed — uses public trade stream.

Usage:
    cd /path/to/flox
    PYTHONPATH=build/python python3 examples/python_ccxt_live.py

Stop with Ctrl+C.
"""

import asyncio
import ccxt.pro as ccxt
import flox_py as flox

# ── Symbols ────────────────────────────────────────────────────────────

registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

# Map ccxt symbol name → flox Symbol.
# Add more symbols here to trade multiple pairs.
SYMBOL_MAP = {
    "BTC/USDT": btc,
}

# ── Strategy ───────────────────────────────────────────────────────────

class SMAStrategy(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)

    def on_start(self):
        print(f"strategy started — watching {btc}")

    def on_stop(self):
        print("strategy stopped")

    def on_trade(self, ctx, trade):
        # trade.symbol is the symbol id — only symbols passed to __init__ arrive here.
        # For a multi-symbol strategy use: if trade.symbol == btc.id: ...
        fv = self.fast.update(trade.price)
        sv = self.slow.update(trade.price)
        if not self.slow.ready:
            return
        if fv > sv and ctx.is_flat():
            self.market_buy(0.01)
        elif fv < sv and ctx.is_flat():
            self.market_sell(0.01)

# ── Signal handler ─────────────────────────────────────────────────────

def on_signal(sig):
    print(f"  signal  {sig.side:4s}  qty={sig.quantity:.4f}  [{sig.order_type}]")
    # In production: submit to exchange via ccxt or gateway:
    #   exchange.create_market_buy_order("BTC/USDT:USDT", sig.quantity)

# ── Runner ─────────────────────────────────────────────────────────────

runner = flox.Runner(registry, on_signal)
runner.add_strategy(SMAStrategy([btc]))
runner.start()

# ── CCXT WebSocket feed ────────────────────────────────────────────────

exchange = ccxt.binance({"newUpdates": True})

async def feed():
    print("connecting to Binance WebSocket...")
    tick_count = 0
    try:
        while True:
            trades = await exchange.watch_trades("BTC/USDT")
            for t in trades:
                sym = SYMBOL_MAP.get(t["symbol"])
                if sym is None:
                    continue
                runner.on_trade(
                    sym,
                    float(t["price"]),
                    float(t["amount"]),
                    t["side"] == "buy",
                    int(t["timestamp"]) * 1_000_000,  # ms → ns
                )
                tick_count += 1
                if tick_count % 50 == 0:
                    print(f"  {tick_count} trades processed  last={t['price']}")
    except asyncio.CancelledError:
        pass
    finally:
        await exchange.close()

async def main():
    feed_task = asyncio.create_task(feed())
    try:
        await asyncio.Event().wait()  # run until Ctrl+C
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        feed_task.cancel()
        await asyncio.gather(feed_task, return_exceptions=True)
        runner.stop()
        print("done")

asyncio.run(main())
