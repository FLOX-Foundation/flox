"""Trailing-stop scenario for the replay-equivalence CI gate.

Frozen on purpose: any change here forces a regeneration of the
expected output (run scripts/replay_equivalence_gate.py --regen).

Enters long on the first trade (fills at 100.00) and arms a SELL
trailing_stop with a fixed 0.30 offset. The trigger ratchets up
behind the tape's rally (100.50 -> 101.00 -> 101.25 -> 101.50 puts
the trigger at 101.20) and the pullback to 101.10 <= 101.20 fires
it: market sell fills at 101.10. Exercises the ratchet math and the
trailing trigger evaluation deterministically.
"""
from __future__ import annotations

import flox_py as flox


class TrailingStopScenario(flox.Strategy):
    def __init__(self, symbols, qty: float = 0.25):
        super().__init__(symbols)
        self.qty = qty
        self._entered = False

    def on_trade(self, ctx, trade):
        if self._entered:
            return
        self._entered = True
        self.market_buy(self.qty)
        self.trailing_stop("sell", 0.30, self.qty)
