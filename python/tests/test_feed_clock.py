"""Tests for the latency-aware multi-feed wait clock."""
from __future__ import annotations

import pytest

from flox_py.feed_clock import (
    FireOnAny, LeaderFollower, MultiFeedClock, WaitForAll,
)


BTC, ETH = 1, 2


def test_wait_for_all_fires_when_both_seen() -> None:
    c = MultiFeedClock(symbols=[BTC, ETH], policy=WaitForAll, timeout_ms=200)
    s = c.tick(1000, BTC)
    assert not s.fired
    s = c.tick(1100, ETH)
    assert s.fired
    assert s.triggered_by == ETH


def test_wait_for_all_resets_after_fire() -> None:
    c = MultiFeedClock(symbols=[BTC, ETH], policy=WaitForAll)
    c.tick(1000, BTC)
    c.tick(1100, ETH)  # fires; resets
    s = c.tick(1200, BTC)
    assert not s.fired
    s = c.tick(1300, ETH)
    assert s.fired


def test_wait_for_all_timeout_path() -> None:
    c = MultiFeedClock(symbols=[BTC, ETH], policy=WaitForAll, timeout_ms=50)
    c.tick(1_000_000, BTC)
    c.tick(2_000_000, ETH)  # initial fire
    # BTC keeps ticking; ETH lags > timeout.
    c.tick(50_000_001, BTC)  # 50ms after last fire? not past budget yet
    s = c.tick(60_000_000, BTC)
    assert s.fired  # past 50ms budget without ETH


def test_fire_on_any_fires_every_tick() -> None:
    c = MultiFeedClock(symbols=[BTC, ETH], policy=FireOnAny)
    assert c.tick(1, BTC).fired
    assert c.tick(2, ETH).fired
    assert c.tick(3, BTC).fired


def test_leader_follower_waits_for_follower_freshness() -> None:
    c = MultiFeedClock(
        symbols=[BTC, ETH], policy=LeaderFollower,
        leader_symbol=BTC, staleness_budget_ms=100,
    )
    # Leader tick before follower has ever ticked => not fresh => no fire.
    s = c.tick(1_000, BTC)
    assert not s.fired
    # Follower ticks; another leader tick within budget => fire.
    c.tick(2_000, ETH)
    s = c.tick(50_000_000, BTC)  # 50ms after follower; under 100ms budget
    assert s.fired


def test_leader_follower_skips_when_follower_too_stale() -> None:
    c = MultiFeedClock(
        symbols=[BTC, ETH], policy=LeaderFollower,
        leader_symbol=BTC, staleness_budget_ms=100,
    )
    c.tick(1_000, ETH)  # follower
    s = c.tick(200_000_000, BTC)  # 200ms after; over 100ms budget
    assert not s.fired


def test_staleness_map_reflects_per_feed_lag() -> None:
    c = MultiFeedClock(symbols=[BTC, ETH], policy=FireOnAny)
    c.tick(1_000_000, BTC)
    c.tick(1_500_000, ETH)
    s = c.tick(2_000_000, BTC)
    assert s.staleness_ns[BTC] == 0  # just ticked
    assert s.staleness_ns[ETH] == 500_000  # 500us lag


def test_out_of_band_symbol_does_not_fire_or_count() -> None:
    c = MultiFeedClock(symbols=[BTC, ETH], policy=WaitForAll)
    s = c.tick(1, 999)  # not in symbol list
    assert not s.fired
    s = c.tick(2, BTC)
    assert not s.fired  # only one of the registered legs has reported
    s = c.tick(3, ETH)
    assert s.fired
