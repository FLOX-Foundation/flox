"""Tests for `flox_py.CompositeBookMatrix`.

Bound from `include/flox/book/composite_book_matrix.h`
(`python/composite_book_bindings.h`). See BOOK-12 in the 2026-09 audit:
`update_book()` previously had no way to distinguish a delta from a
snapshot at all (it always went in as a snapshot), and the underlying
engine, when it does see a delta, used to zero out the untouched side.
"""
from __future__ import annotations

import flox_py

SYMBOL = 1
EXCHANGE = 0


def test_snapshot_sets_both_sides() -> None:
    m = flox_py.CompositeBookMatrix()
    m.update_book(EXCHANGE, SYMBOL, [100.0], [10.0], [100.05], [5.0], 0)

    bid = m.best_bid(SYMBOL)
    ask = m.best_ask(SYMBOL)
    assert bid is not None and bid["price"] == 100.0
    assert ask is not None and ask["price"] == 100.05


def test_bid_only_delta_does_not_wipe_ask_side() -> None:
    m = flox_py.CompositeBookMatrix()
    m.update_book(EXCHANGE, SYMBOL, [100.0], [10.0], [100.05], [5.0], 0)

    # A Bybit-style incremental delta that only carries a bid update.
    m.update_book(EXCHANGE, SYMBOL, [99.90], [3.0], [], [], 0, is_delta=True)

    bid = m.bid_for_exchange(SYMBOL, EXCHANGE)
    ask = m.ask_for_exchange(SYMBOL, EXCHANGE)
    assert bid is not None and bid["price"] == 99.90
    assert ask is not None, "ask side must survive a bid-only delta"
    assert ask["price"] == 100.05


def test_ask_only_delta_does_not_wipe_bid_side() -> None:
    m = flox_py.CompositeBookMatrix()
    m.update_book(EXCHANGE, SYMBOL, [100.0], [10.0], [100.05], [5.0], 0)

    m.update_book(EXCHANGE, SYMBOL, [], [], [100.10], [2.0], 0, is_delta=True)

    bid = m.bid_for_exchange(SYMBOL, EXCHANGE)
    assert bid is not None, "bid side must survive an ask-only delta"
    assert bid["price"] == 100.0


def test_snapshot_with_empty_side_invalidates_that_side() -> None:
    m = flox_py.CompositeBookMatrix()
    m.update_book(EXCHANGE, SYMBOL, [100.0], [10.0], [100.05], [5.0], 0)

    # A genuine snapshot with no ask levels means the ask side is empty.
    m.update_book(EXCHANGE, SYMBOL, [101.0], [1.0], [], [], 0, is_delta=False)

    assert m.ask_for_exchange(SYMBOL, EXCHANGE) is None
