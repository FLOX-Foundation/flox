"""`TradeData.exchange_ts_ns` carries the trade's timestamp in `on_trade`.

The reference documents both `timestamp_ns` and `exchange_ts_ns` on the
trade a strategy receives. The callback filled only `timestamp_ns`, so a
strategy that read the documented exchange field saw 0 on every trade.
"""
from __future__ import annotations

import lrvx

TS_NS = 1_788_220_820_000_000_000  # 2026-09-01T00:00:20Z


def test_on_trade_carries_the_exchange_timestamp():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTC", 0.01)
    seen = []
    runner = lrvx.Runner(reg, lambda s: None, threaded=False)

    class S(lrvx.Strategy):
        def on_trade(self, ctx, trade):
            seen.append((int(trade.timestamp_ns), int(trade.exchange_ts_ns)))

    runner.add_strategy(S(symbols=[sym]))
    runner.start()
    runner.on_trade(sym, 100.0, 1.0, True, TS_NS)
    runner.on_trade(sym, 100.5, 0.5, False, TS_NS + 1)
    runner.stop()

    assert seen == [(TS_NS, TS_NS), (TS_NS + 1, TS_NS + 1)], seen
