"""Reference strategy for the replay-equivalence CI gate.

Frozen on purpose: any change here forces a regeneration of the
expected output (run scripts/replay_equivalence_gate.py --regen).

The strategy fires one MARKET BUY on the first observed trade and
holds — small enough that the gate is fast, exercises the
strategy → simulator → fill round trip end-to-end.
"""
from __future__ import annotations

import flox_py as flox


class ReplayEquivalenceStrategy(flox.Strategy):
    def __init__(self, symbols, qty: float = 0.25):
        super().__init__(symbols)
        self.qty = qty
        self._fired = False

    def on_trade(self, ctx, trade):
        if self._fired:
            return
        self._fired = True
        self.market_buy(self.qty)
