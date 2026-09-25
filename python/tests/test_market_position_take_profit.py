"""A fired take-profit limit reports its position like a plain limit.

Once a take-profit limit crosses its trigger it posts its limit price and
becomes a resting limit order, and ``Strategy.on_market_position_change``
is how a strategy watches a resting order -- where it sits relative to
best, how far off it is, when that changes. An exit that has armed and is
now waiting is the order a strategy most wants to watch.

``fillsAsLimit`` (``src/backtest/simulated_executor.cpp``) lists LIMIT and
STOP_LIMIT and gates the queue tracker's registration,
``driveQueueFromBarStep`` and ``maybeEmitMarketPositionChanges`` on that
list; TAKE_PROFIT_LIMIT is missing from it. The fill paths gate on
``!fillsAsMarket`` instead, which is why a take-profit limit still fills
correctly and only its reporting is affected -- and why nothing caught it.

What that costs is visible only once a queue model is configured: the
unregistered order's own level looks empty to ``computeMarketPosition``
however much depth rests there, and it reports ``level_empty`` where a
plain limit beside it reports ``best``. That case is pinned in
``tests/test_market_position.cpp``
(``MarketPosition.FiredTakeProfitLimitReportsLikeAPlainLimit``), because
no Python entry point can put a queue model in front of a strategy:
``SimulatedExecutor.set_queue_model`` exists but neither
``Runner.set_executor`` nor ``BacktestRunner.set_executor`` accepts a
``SimulatedExecutor`` -- both take the hook-style ``Executor`` or a
``VenueExecutor``, and ``VenueExecutor`` has no ``set_queue_model``.

These pass today. They guard the half of the behaviour the binding can
reach: with no queue model the two order types report the same sequence,
and a fix for the C++ case must not change that.
"""
from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import flox_py as flox
from flox_py import tape

NS = 1_000_000_000

# The book walks its ask in front of the resting sell at 101.0 and away
# again, so a resting order has categorical transitions to report. Every
# tick also prints at 100.6, which crosses the take-profit's 100.5
# trigger on the first one: both orders are live from the same tick, so
# the two runs are comparable event for event.
BOOK = ((100.0, 101.0), (100.0, 101.0), (100.0, 100.8), (100.0, 101.5))
TRADE_PRICE = 100.6
REST_PRICE = 101.0
TRIGGER = 100.5


def _record_tape(work: Path) -> Path:
    out = work / "tape"
    out.mkdir()
    registry = flox.SymbolRegistry()
    sym = int(registry.add_symbol("sim", "BTCUSDT", tick_size=0.01))

    recorder = tape.make_recorder_hook(out)
    runner = flox.Runner(registry, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    ts = 0
    for bid, ask in BOOK:
        ts += NS
        runner.on_book_snapshot(sym, [bid, bid - 0.1], [5.0, 5.0],
                                [ask, ask + 0.1], [5.0, 5.0], ts)
        runner.on_trade(sym, TRADE_PRICE, 1.0, True, ts)
    runner.stop()
    recorder.close()
    return out


class _Watcher(flox.Strategy):
    """Rests one sell at REST_PRICE and records what it is told about it."""

    def __init__(self, symbols, kind: str):
        super().__init__(symbols)
        self.kind = kind
        self.armed = False
        self.positions: list[tuple[str, str, int]] = []

    def on_trade(self, ctx, trade):
        if self.armed:
            return
        self.armed = True
        if self.kind == "limit":
            self.limit_sell(price=REST_PRICE, qty=1.0)
        else:
            self.take_profit_limit(side="sell", trigger=TRIGGER,
                                   limit_price=REST_PRICE, qty=1.0)

    def on_market_position_change(self, ctx, ev):
        self.positions.append(
            (ev.order_type, ev.market_position, ev.distance_to_best_ticks))


def _run(kind: str, tape_dir: Path) -> list[tuple[str, str, int]]:
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("sim", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, fee_rate=0.0, initial_capital=100_000.0)
    strat = _Watcher([sym], kind)
    runner.set_strategy(strat)
    runner.run_tape(str(tape_dir))
    return strat.positions


def _both() -> tuple[list, list]:
    work = Path(tempfile.mkdtemp(prefix="flox-mp-tp-test-"))
    try:
        tape_dir = _record_tape(work)
        return _run("limit", tape_dir), _run("take_profit_limit", tape_dir)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def test_a_plain_limit_reports_its_position() -> None:
    plain, _ = _both()
    assert plain, "the control reported nothing -- the harness is broken"
    assert plain[0][0] == "limit"


def test_a_fired_take_profit_limit_reports_its_position() -> None:
    _, take_profit = _both()
    assert take_profit, (
        "a fired take-profit limit rests like any other limit and reported "
        "no position at all"
    )
    assert take_profit[0][0] == "tp_limit"


def test_both_report_the_same_sequence() -> None:
    plain, take_profit = _both()
    assert [(p, d) for _, p, d in take_profit] == [(p, d) for _, p, d in plain]
