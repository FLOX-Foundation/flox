"""Regression test: BacktestRunner.run_bars must not re-guess the unit of
start_time_ns / end_time_ns.

Those arrays are nanoseconds by contract (the kwarg names say so, and
docs/bindings/python.md documents run_bars as taking already-nanosecond
timestamps with no unit guessing, unlike run_csv / run_ohlcv). The old
normalizeTs() ran every value through a magnitude-based seconds/ms/us/ns
guess anyway. Any bar timestamp under 1e12 -- which is every bar in an
ordinary few-minute 1-minute series, since 1e12ns is ~11.5 days -- got
misread as seconds and rescaled by another 1e9, overflowing int64. That
overflow is undefined behavior: a silent wrong value in a normal build, a
trap under -fsanitize=undefined. Neither is acceptable for a value the API
already declared to be in nanoseconds.
"""
from __future__ import annotations

import flox_py as flox
import numpy as np


class _BuyThenSell(flox.Strategy):
    """Emits on bar 1 and bar 2, so a single closed trade pins both
    entry_time_ns and exit_time_ns against the exact bar timestamps that
    produced them. The two orders fill one bar later than they are emitted;
    see the comment on the expected values below."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.bar_count = 0

    def on_bar(self, ctx, bar):
        if self.bar_count == 1:
            self.market_buy(0.5)
        elif self.bar_count == 2:
            self.market_sell(0.5)
        self.bar_count += 1


def _make_minute_bars(n: int):
    # Every value here is well under 1e12 (1e12ns is ~11.5 days; this is a
    # few minutes), which is exactly the range the old heuristic mistook
    # for seconds-since-epoch.
    start_ns = np.array([i * 60_000_000_000 for i in range(n)], dtype=np.int64)
    end_ns = start_ns + 60_000_000_000
    open_ = np.full(n, 100.0)
    high = open_ + 0.5
    low = open_ - 0.5
    close = open_ + 0.25
    volume = np.full(n, 100.0)
    return start_ns, end_ns, open_, high, low, close, volume


def test_run_bars_preserves_exact_nanosecond_timestamps() -> None:
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, initial_capital=100_000.0)
    strat = _BuyThenSell([sym])
    runner.set_strategy(strat)

    n = 5
    bars = _make_minute_bars(n)
    start_ns, end_ns = bars[0], bars[1]
    runner.run_bars(*bars, symbol="BTCUSDT")

    trades = runner.trades()
    assert len(trades["entry_time_ns"]) == 1, (
        f"expected exactly one closed trade, got {len(trades['entry_time_ns'])}"
    )

    # An order emitted from a bar callback cannot trade on the bar the
    # strategy was shown -- every price in it is already past -- so it is
    # held until the next bar opens. The buy emitted on bar 1 therefore
    # fills during bar 2, and the sell emitted on bar 2 fills during bar 3.
    # The runner advances its clock to a bar's end_time_ns before walking
    # that bar, so the two fills carry end_ns[2] and end_ns[3]: values taken
    # from the input array with no arithmetic in between, which is the whole
    # point of this test.
    expected_entry_ns = int(end_ns[2])
    expected_exit_ns = int(end_ns[3])
    actual_entry_ns = int(trades["entry_time_ns"][0])
    actual_exit_ns = int(trades["exit_time_ns"][0])

    assert actual_entry_ns == expected_entry_ns, (
        f"entry_time_ns should be exactly {expected_entry_ns} (bar 2's "
        f"end_time_ns, unmodified); got {actual_entry_ns}. A timestamp "
        "under 1e12 coming back rescaled means the magnitude-based unit "
        "guess is back on a value that was already nanoseconds."
    )
    assert actual_exit_ns == expected_exit_ns, (
        f"exit_time_ns should be exactly {expected_exit_ns} (bar 3's "
        f"end_time_ns, unmodified); got {actual_exit_ns}."
    )

    # Sanity bound distinguishing "correct" from "silently overflowed":
    # every legitimate timestamp here fits well under 2^63 nanoseconds
    # (roughly the year 2262). The old code could multiply a ~1e11ns
    # value by another 1e9, which overflows int64_t (undefined behavior --
    # in practice a huge negative or wrapped-around magnitude, not a
    # plausible timestamp).
    max_plausible_ns = 10**12
    assert 0 <= actual_entry_ns < max_plausible_ns
    assert 0 <= actual_exit_ns < max_plausible_ns
