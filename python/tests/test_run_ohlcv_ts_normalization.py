"""Regression test: BacktestRunner.run_ohlcv must not re-guess the unit of
its `ts` array.

`ts` is nanoseconds by contract -- the same contract
flox_backtest_runner_run_ohlcv's timestamps_ns C ABI parameter documents,
that Node's runOhlcv uses (a BigInt64Array passed straight through to that
same C ABI call, no rescaling), and that Codon's run_ohlcv documents
("timestamps in nanoseconds"). The Python binding used to be the one
outlier: it ran every value through normalizeTs(), the same
magnitude-based seconds/ms/us/ns guess run_csv legitimately uses for a
raw, genuinely-unit-less CSV column. Any ts under 1e12 -- an ordinary
value for synthetic or offset test data, ~11.5 days of nanoseconds -- got
misread as seconds and rescaled by another 1e9, overflowing int64_t. That
overflow is undefined behavior: silently wrong in a normal build, a trap
under -fsanitize=undefined.

See python/tests/test_run_bars_ts_normalization.py for the identical
hazard on run_bars, fixed earlier in the same audit.
"""
from __future__ import annotations

import flox_py as flox


class _RecordTradeTimes(flox.Strategy):
    """Records every on_trade timestamp_ns it observes, unmodified, so the
    test can compare the values run_ohlcv actually dispatched against the
    exact ts[] it was given."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.seen_ns = []

    def on_trade(self, ctx, trade):
        self.seen_ns.append(int(trade.timestamp_ns))


def test_run_ohlcv_preserves_exact_nanosecond_timestamps() -> None:
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, initial_capital=100_000.0)
    strat = _RecordTradeTimes([sym])
    runner.set_strategy(strat)

    # Every value here is well under 1e12 (1e12ns is ~11.5 days; this is a
    # few minutes), which is exactly the range the old heuristic mistook
    # for seconds-since-epoch and rescaled by another 1e9.
    n = 5
    ts = [i * 60_000_000_000 for i in range(n)]
    close = [100.0 + i * 0.25 for i in range(n)]

    runner.run_ohlcv(ts, close, symbol="BTCUSDT")

    assert strat.seen_ns == ts, (
        f"on_trade should see run_ohlcv's ts[] values exactly as given "
        f"(nanoseconds by contract, no unit guessing); expected {ts}, got "
        f"{strat.seen_ns}. A value coming back rescaled means the "
        "magnitude-based unit guess is back on a value that was already "
        "nanoseconds."
    )

    # Sanity bound distinguishing "correct" from "silently overflowed":
    # every legitimate timestamp here fits well under 2^63 nanoseconds
    # (roughly the year 2262). The old code could multiply a value like
    # 60_000_000_000 (bar 2, exactly the "60 seconds in nanoseconds" case
    # cited when this was found) by another 1e9, which overflows
    # int64_t -- undefined behavior, in practice a huge or wrapped-around
    # magnitude, not a plausible timestamp.
    max_plausible_ns = 10**12
    for observed in strat.seen_ns:
        assert 0 <= observed < max_plausible_ns
