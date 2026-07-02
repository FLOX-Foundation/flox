"""Stop-loss scenario for the replay-equivalence CI gate.

Frozen on purpose: any change here forces a regeneration of the
expected output (run scripts/replay_equivalence_gate.py --regen).

Enters long on the first trade (fills at 100.00), then arms a
protective SELL stop_market at 101.20 once the tape prints 101.50.
The next trade in the frozen tape is 101.10 <= 101.20, so the stop
triggers, converts to a market order and fills at 101.10. Exercises
the conditional-order path end-to-end: signal emission with a
trigger price, trigger evaluation against the trade feed, and the
triggered-market fill.
"""
from __future__ import annotations

import flox_py as flox


class StopLossScenario(flox.Strategy):
    def __init__(self, symbols, qty: float = 0.25):
        super().__init__(symbols)
        self.qty = qty
        self._entered = False
        self._stop_armed = False

    def on_trade(self, ctx, trade):
        if not self._entered:
            self._entered = True
            self.market_buy(self.qty)
            return
        if not self._stop_armed and trade.price >= 101.50:
            self._stop_armed = True
            self.stop_market("sell", 101.20, self.qty)
