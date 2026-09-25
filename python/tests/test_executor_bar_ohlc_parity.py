"""The bar path of the pybind11 binding against the bar path of the engine.

`BacktestRunner.run_bars` walks every bar open -> low -> high -> close, holds
whatever the bar callback submits, and releases it at the next bar's open. A
caller driving `SimulatedExecutor` by hand has none of that: the binding
exposes `on_bar(symbol, close)` and nothing else, so it moves the market
straight to the close, never sees an intrabar extreme, and matches a callback
order at a price that exists only because the bar has already happened.

Needed on `SimulatedExecutor`:

    on_bar_ohlc(symbol: int, open: float, high: float, low: float, close: float) -> None
    begin_bar_callback_window() -> None
    end_bar_callback_window() -> None
    reset() -> None

and `close_reason` on the bars `aggregate_*_bars` returns.

Sister cases live in tests/test_capi_executor_bar_ohlc.cpp and
node/test/test_executor_bar_ohlc_parity.js against the same tape.
"""
from __future__ import annotations

import numpy as np
import pytest

import flox_py as flox

MINUTE_NS = 60_000_000_000
SYMBOL = 1

# Bar 0 closes at 105 and bar 1 opens at 106 -- a price that appears nowhere in
# bar 0, so a fill at the open is distinguishable from a fill at anything the
# bar-0 callback was shown. Bar 1 runs its high to 109.5, above the resting
# sell at 109 and above its own close of 107, so a fill there can only come
# from the intrabar extreme.
TAPE = [
    (100.0, 110.0, 90.0, 105.0),
    (106.0, 109.5, 104.0, 107.0),
    (103.0, 104.0, 102.0, 103.5),
]

# (side, price, quantity) -- order ids differ between the two paths because the
# runner mints its own.
EXPECTED_FILLS = [("buy", 106.0, 1.0), ("sell", 109.0, 1.0)]

_BAR_API = {
    "on_bar_ohlc": "on_bar_ohlc(symbol, open, high, low, close)",
    "begin_bar_callback_window": "begin_bar_callback_window()",
    "end_bar_callback_window": "end_bar_callback_window()",
    "reset": "reset()",
}


def _missing(executor) -> list[str]:
    return [sig for name, sig in _BAR_API.items() if not hasattr(executor, name)]


def _require_bar_api(executor) -> None:
    missing = _missing(executor)
    if missing:
        pytest.fail(
            "SimulatedExecutor is missing the bar path: "
            + ", ".join(missing)
            + " -- a caller driving the executor by hand gets neither the "
            "bar's open nor its intrabar extremes"
        )


def _bar_arrays():
    start = np.array([i * MINUTE_NS for i in range(len(TAPE))], dtype=np.int64)
    end = start + MINUTE_NS
    cols = [np.array([row[i] for row in TAPE], dtype=np.float64) for i in range(4)]
    volume = np.full(len(TAPE), 1000.0, dtype=np.float64)
    return start, end, cols[0], cols[1], cols[2], cols[3], volume


class _BarZeroStrategy(flox.Strategy):
    """Sends both orders from the callback of bar 0, nothing after."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.bars = 0
        self.fills: list[tuple[str, float, float]] = []

    def on_bar(self, ctx, bar):  # noqa: ARG002
        if self.bars == 0:
            self.market_buy(1.0)
            self.limit_sell(109.0, 1.0)
        self.bars += 1

    def on_fill(self, ctx, ev):  # noqa: ARG002
        self.fills.append((ev.side, ev.fill_price, ev.fill_qty))


def _runner_fills() -> list[tuple[str, float, float]]:
    registry = flox.SymbolRegistry()
    registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, fee_rate=0.0, initial_capital=100_000.0)
    strategy = _BarZeroStrategy([SYMBOL])
    runner.set_strategy(strategy)
    runner.run_bars(*_bar_arrays(), symbol="BTCUSDT")
    return strategy.fills


def _drive_by_hand(executor) -> None:
    """Exactly what run_bars does per bar, spelled through the binding."""
    sent = False
    for i, (open_, high, low, close) in enumerate(TAPE):
        executor.advance_clock((i + 1) * MINUTE_NS)
        executor.on_bar_ohlc(SYMBOL, open_, high, low, close)

        executor.begin_bar_callback_window()
        if not sent:
            sent = True
            executor.submit_order(1, "buy", 0.0, 1.0, "market", SYMBOL)
            executor.submit_order(2, "sell", 109.0, 1.0, "limit", SYMBOL)
        executor.end_bar_callback_window()


def _fill_rows(executor) -> list[tuple[str, float, float]]:
    return [(f["side"], f["price"], f["quantity"]) for f in executor.fills_list()]


def test_the_runner_path_is_the_reference() -> None:
    """Green control: the engine's own answer for this tape, through the
    binding that already reaches the full bar path."""
    assert _runner_fills() == EXPECTED_FILLS


def test_simulated_executor_exposes_the_bar_path() -> None:
    missing = _missing(flox.SimulatedExecutor())
    assert missing == [], (
        "SimulatedExecutor is missing "
        + ", ".join(missing)
        + "; on_bar(symbol, close) is the whole of the bar path a caller can "
        "reach today"
    )


def test_hand_driven_bars_match_the_runner() -> None:
    executor = flox.SimulatedExecutor()
    _require_bar_api(executor)
    _drive_by_hand(executor)
    assert _fill_rows(executor) == _runner_fills()


def test_a_held_order_fills_at_the_open_not_the_close() -> None:
    """The mutation this is shaped against: on_bar_ohlc forwarding the close
    where the open belongs."""
    executor = flox.SimulatedExecutor()
    _require_bar_api(executor)
    _drive_by_hand(executor)

    rows = _fill_rows(executor)
    assert rows, "the market buy never filled"
    side, price, _qty = rows[0]
    assert side == "buy"
    assert price == 106.0, "a callback order fills at the next bar's open"
    assert price != 105.0, "filled at the close of the bar the callback was shown"
    assert price != 100.0, "filled at the open of a bar that had already closed"


def test_a_resting_order_matches_the_intrabar_extreme() -> None:
    executor = flox.SimulatedExecutor()
    _require_bar_api(executor)
    _drive_by_hand(executor)

    rows = _fill_rows(executor)
    assert len(rows) == 2, f"the resting sell never matched the bar high: {rows}"
    side, price, _qty = rows[1]
    assert side == "sell"
    assert price == 109.0, "a resting limit trades at the price it posted"


def test_without_the_window_the_order_is_not_held() -> None:
    executor = flox.SimulatedExecutor()
    _require_bar_api(executor)

    open_, high, low, close = TAPE[0]
    executor.advance_clock(MINUTE_NS)
    executor.on_bar_ohlc(SYMBOL, open_, high, low, close)
    executor.submit_order(1, "buy", 0.0, 1.0, "market", SYMBOL)

    rows = _fill_rows(executor)
    assert len(rows) == 1, "an order submitted outside the window was held"
    assert rows[0][1] == close, "outside the window the market is already at the close"


def test_reset_makes_a_second_run_report_that_run() -> None:
    executor = flox.SimulatedExecutor()
    _require_bar_api(executor)

    _drive_by_hand(executor)
    first = _fill_rows(executor)
    assert first

    executor.reset()
    assert executor.fill_count == 0, "reset left the previous run's fills behind"

    _drive_by_hand(executor)
    assert _fill_rows(executor) == first, "the second run did not repeat the first"


def test_aggregated_bars_carry_a_close_reason() -> None:
    """Batch aggregation hands back bars with no `close_reason`, so the reason
    a bar closed is readable on the live callback path (`BarData`) and nowhere
    else."""
    ts = np.array(
        [0, 30_000_000_000, 61_000_000_000, 91_000_000_000, 121_000_000_000],
        dtype=np.int64,
    )
    prices = np.array([100.0, 101.0, 102.0, 103.0, 104.0], dtype=np.float64)
    quantities = np.ones(5, dtype=np.float64)
    is_buy = np.ones(5, dtype=np.uint8)

    bars = flox.aggregate_time_bars(ts, prices, quantities, is_buy, 60.0)
    names = bars.dtype.names or ()
    assert "close_reason" in names, (
        "aggregated bars carry no close_reason; fields are "
        f"{list(names)}. flox.BarData already has one on the live path"
    )
    # 0 = Threshold: every bar the batch path returns was closed by its own
    # threshold, and the field has to say so rather than report a leftover.
    assert [int(b["close_reason"]) for b in bars] == [0] * len(bars)
