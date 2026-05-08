"""Latency-aware multi-feed wait policies.

Cross-symbol decisions read from N feeds. Feeds arrive at different
rates and with different jitter, so a strategy that recomputes a
ratio on every BTC tick is using a stale ETH price every time the
ETH feed lags. This module ships explicit "wait for both legs up
to N ms" policies so the staleness budget is part of the strategy,
not a silent timing artifact.

Example::

    from flox_py.feed_clock import MultiFeedClock, WaitForAll

    class PairTrade(Strategy):
        def setup(self):
            self.clock = MultiFeedClock(
                symbols=[BTC, ETH],
                policy=WaitForAll,
                timeout_ms=200,
            )

        def on_trade(self, ctx, trade):
            state = self.clock.tick(trade.exchange_ts_ns, ctx.symbol_id)
            if state.fired:
                self.evaluate(state)
"""
from __future__ import annotations

import enum
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional


class Policy(enum.IntEnum):
    WAIT_FOR_ALL = 0
    FIRE_ON_ANY = 1
    LEADER_FOLLOWER = 2


# Module-level constant aliases mirror the tracker spec.
WaitForAll = Policy.WAIT_FOR_ALL
FireOnAny = Policy.FIRE_ON_ANY
LeaderFollower = Policy.LEADER_FOLLOWER


@dataclass
class ClockState:
    """Returned by `MultiFeedClock.tick()`. `fired` indicates the
    strategy should evaluate now; the per-feed staleness map lets the
    strategy down-weight or skip a decision based on how stale a leg is."""
    fired: bool
    last_ts_ns: Dict[int, int] = field(default_factory=dict)
    staleness_ns: Dict[int, int] = field(default_factory=dict)
    triggered_by: Optional[int] = None


@dataclass
class MultiFeedClock:
    symbols: List[int]
    policy: Policy = WaitForAll
    timeout_ms: int = 200
    leader_symbol: Optional[int] = None
    staleness_budget_ms: int = 200

    _last_seen: Dict[int, int] = field(default_factory=dict, init=False)
    _last_fire_ts: int = field(default=0, init=False)
    _seen_since_fire: set = field(default_factory=set, init=False)

    def tick(self, ts_ns: int, symbol_id: int) -> ClockState:
        """Update the clock with one feed event. Returns a `ClockState`
        whose `fired` flag tells the caller whether to act."""
        if symbol_id not in self.symbols:
            # Out-of-band symbol — record but never fire on its own.
            self._last_seen[symbol_id] = ts_ns
            return self._snapshot(ts_ns, fired=False, triggered_by=None)

        self._last_seen[symbol_id] = ts_ns
        self._seen_since_fire.add(symbol_id)
        fired = False
        if self.policy == WaitForAll:
            if all(s in self._seen_since_fire for s in self.symbols):
                fired = True
            elif self._last_fire_ts > 0:
                # Timeout fallback — fire even if not all feeds reported,
                # but only if we have been waiting longer than the budget.
                budget_ns = self.timeout_ms * 1_000_000
                if ts_ns - self._last_fire_ts > budget_ns:
                    fired = True
        elif self.policy == FireOnAny:
            fired = True
        elif self.policy == LeaderFollower:
            leader = self.leader_symbol if self.leader_symbol is not None else self.symbols[0]
            if symbol_id == leader:
                # Fire on leader tick only if every follower's staleness
                # is within budget.
                budget_ns = self.staleness_budget_ms * 1_000_000
                followers_fresh = True
                for s in self.symbols:
                    if s == leader:
                        continue
                    last = self._last_seen.get(s)
                    if last is None or ts_ns - last > budget_ns:
                        followers_fresh = False
                        break
                fired = followers_fresh

        if fired:
            self._last_fire_ts = ts_ns
            self._seen_since_fire.clear()
        return self._snapshot(ts_ns, fired=fired, triggered_by=symbol_id)

    def _snapshot(self, ts_ns: int, fired: bool, triggered_by: Optional[int]) -> ClockState:
        last = dict(self._last_seen)
        stale = {s: max(0, ts_ns - last.get(s, ts_ns)) for s in self.symbols}
        return ClockState(fired=fired, last_ts_ns=last, staleness_ns=stale,
                           triggered_by=triggered_by)

    def reset(self) -> None:
        self._last_seen.clear()
        self._last_fire_ts = 0
        self._seen_since_fire.clear()
