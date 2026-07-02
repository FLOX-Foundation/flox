"""Take-profit scenario for the replay-equivalence CI gate.

Frozen on purpose: any change here forces a regeneration of the
expected output (run scripts/replay_equivalence_gate.py --regen).

Enters long on the first trade (fills at 100.00) and immediately
arms a SELL take_profit_market at 101.30. The tape prints 101.25
first (below the trigger — must not fire; pins the boundary), then
101.50 >= 101.30 fires the take-profit, which converts to a market
order and fills at 101.50.
"""
from __future__ import annotations

import flox_py as flox


class TakeProfitScenario(flox.Strategy):
    def __init__(self, symbols, qty: float = 0.25):
        super().__init__(symbols)
        self.qty = qty
        self._entered = False

    def on_trade(self, ctx, trade):
        if self._entered:
            return
        self._entered = True
        self.market_buy(self.qty)
        self.take_profit_market("sell", 101.30, self.qty)
