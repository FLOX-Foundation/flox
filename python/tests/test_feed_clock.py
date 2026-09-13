"""Tests for the MultiFeedClock primitive.

`flox_py.MultiFeedClock` is bound from
`include/flox/feed/multi_feed_clock.h` and is the same primitive every
binding (pybind11 / NAPI / QuickJS / Codon) reaches through the C ABI.
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


def test_wait_for_all_timeout_fallback_fires_before_any_full_fire() -> None:
    # BOOK-11: the timeout fallback used to only work *after* the clock had
    # already fired once (it checked "last fire timestamp > 0", which is
    # indistinguishable from "never fired"). ETH never ticks here at all,
    # so WaitForAll's normal condition is never met -- only the timeout can
    # ever fire.
    c = flox_py.MultiFeedClock(symbols=[BTC, ETH],
                                policy=flox_py.FeedClockPolicy.WAIT_FOR_ALL,
                                timeout_ms=200)

    fired_count = 0
    for i in range(1000):
        result = c.tick(i * (SECOND_NS // 10), BTC)  # one BTC tick every 100ms
        if result["fired"]:
            fired_count += 1

    assert fired_count > 0, "the 200ms timeout must fall back to firing even " \
        "though the second feed never ticks"


def test_wait_for_all_timeout_fallback_survives_a_zero_timestamp_first_fire() -> None:
    # BOOK-11, beyond the original report: if the first full WaitForAll
    # fire happens to land exactly on ts_ns == 0 (a backtest replayed from
    # a zero-based clock), the old check disabled the timeout permanently.
    c = flox_py.MultiFeedClock(symbols=[BTC, ETH],
                                policy=flox_py.FeedClockPolicy.WAIT_FOR_ALL,
                                timeout_ms=200)

    assert c.tick(0, BTC)["fired"] is False
    assert c.tick(0, ETH)["fired"] is True  # full fire, at ts_ns == 0

    fired_count = 0
    for i in range(1, 11):
        if c.tick(i * 300_000_000, BTC)["fired"]:  # ETH silent, BTC every 300ms
            fired_count += 1

    assert fired_count == 10


def test_out_of_band_symbol_does_not_fire_or_affect_registered_state() -> None:
    # docs/how-to/multi-feed-clock.md used to claim an out-of-band tick
    # "updates the per-symbol last-seen timestamp". It never did -- the
    # implementation returns before touching any state for a symbol that
    # was not in the constructor's `symbols` list.
    c = flox_py.MultiFeedClock(symbols=[BTC, ETH],
                                policy=flox_py.FeedClockPolicy.WAIT_FOR_ALL,
                                timeout_ms=200)

    UNREGISTERED = 99
    c.tick(SECOND_NS, BTC)
    oob = c.tick(SECOND_NS + 1, UNREGISTERED)
    assert oob["fired"] is False
    assert oob["triggered_by"] == UNREGISTERED

    after = c.tick(SECOND_NS + 2, BTC)
    assert after["last_ts_ns"][BTC] == SECOND_NS + 2
