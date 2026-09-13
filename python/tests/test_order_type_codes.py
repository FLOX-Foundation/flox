"""Regression test for the order-type code spaces.

Python's ``SimulatedExecutor.submit_order`` already parses the order-type
string symbolically (``python/backtest_bindings.h:parseOrderType``), unlike
Node's, which used to encode "market"/"limit" in the wrong code space. This
pins the same reference behavior Node is tested against so a future
regression here is caught too: a market buy fills on the first print,
and a limit buy priced below the market does not fill until the market
reaches it.
"""
from __future__ import annotations

import flox_py as flox


def test_market_fills_on_first_print_limit_waits_for_price():
    exe = flox.SimulatedExecutor()
    sym = 1

    exe.submit_order(1, "buy", 1.0, 1.0, "market", sym)
    assert exe.fill_count == 0, "market order must not fill before any print"
    exe.on_bar(sym, 100.0)
    assert exe.fill_count == 1, "market buy must fill on the first print"

    exe.submit_order(2, "buy", 50.0, 1.0, "limit", sym)
    exe.on_bar(sym, 100.0)
    assert exe.fill_count == 1, "limit at 50 must not fill while market prints at 100"
    exe.on_bar(sym, 50.0)
    assert exe.fill_count == 2, "limit at 50 must fill once the market reaches 50"
