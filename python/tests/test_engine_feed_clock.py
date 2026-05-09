"""Tests for the C++-backed MultiFeedClock primitive (W6-T021).

The legacy Python helper lives in `flox_py.feed_clock.MultiFeedClock`.
This suite exercises the new `flox_py.MultiFeedClock` (bound from
`include/flox/feed/multi_feed_clock.h`), which the NAPI / QuickJS /
Codon bindings share through the C ABI.
"""
from __future__ import annotations

import flox_py


BTC, ETH = 1, 2
SECOND_NS = 1_000_000_000


def test_wait_for_all_fires_after_both_feeds() -> None:
    c = flox_py.MultiFeedClock(symbols=[BTC, ETH],
                                policy=flox_py.FeedClockPolicy.WAIT_FOR_ALL,
                                timeout_ms=200)
    r1 = c.tick(SECOND_NS, BTC)
    assert r1["fired"] is False

    r2 = c.tick(SECOND_NS + 100_000_000, ETH)
    assert r2["fired"] is True
    assert r2["triggered_by"] == ETH
    assert r2["staleness_ns"][BTC] == 100_000_000
    assert r2["staleness_ns"][ETH] == 0

    # After fire the accumulator resets.
    r3 = c.tick(SECOND_NS + 200_000_000, BTC)
    assert r3["fired"] is False


def test_fire_on_any_fires_every_tick() -> None:
    c = flox_py.MultiFeedClock(symbols=[BTC, ETH],
                                policy=flox_py.FeedClockPolicy.FIRE_ON_ANY)
    assert c.tick(SECOND_NS, BTC)["fired"] is True
    assert c.tick(SECOND_NS + 1, ETH)["fired"] is True


def test_leader_follower_requires_fresh_follower() -> None:
    c = flox_py.MultiFeedClock(symbols=[BTC, ETH],
                                policy=flox_py.FeedClockPolicy.LEADER_FOLLOWER,
                                leader_symbol=BTC,
                                staleness_budget_ms=200)
    assert c.tick(SECOND_NS, ETH)["fired"] is False
    assert c.tick(SECOND_NS + 50_000_000, BTC)["fired"] is True
    # Stale follower (>200ms) blocks the leader fire.
    assert c.tick(SECOND_NS + 500_000_000, BTC)["fired"] is False
