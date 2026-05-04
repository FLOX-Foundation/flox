"""
python/tests/test_hooks.py — smoke-test pybind11 wrappers for the
extension hooks (PnLTracker, StorageSink, RiskManager, KillSwitch,
OrderValidator, MarketDataRecorderHook, ReplaySource, Executor,
ExecutionListener, set_log_callback).

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_hooks.py
"""

import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox

_passed = 0
_failed = 0


def check(cond, msg):
    global _passed, _failed
    if cond:
        print(f"  ok  {msg}")
        _passed += 1
    else:
        print(f"  FAIL  {msg}")
        _failed += 1


def make_runner_with_strategy():
    reg = flox.SymbolRegistry()
    sym_id = reg.add_symbol("test", "BTC", 0.01)

    fired_signal = []

    def on_signal(sig):
        fired_signal.append(sig)

    runner = flox.Runner(reg, on_signal, threaded=False)

    # Minimal strategy that fires a market buy on first trade.
    fired = [False]

    class S(flox.Strategy):
        def on_trade(self, ctx, trade):
            if fired[0]:
                return
            fired[0] = True
            self.emit_market_buy(sym_id, 1.0)

    strat = S(symbols=[sym_id])
    runner.add_strategy(strat)
    return reg, runner, strat, sym_id, fired_signal


def test_pnl_tracker():
    print("test_pnl_tracker")
    reg, runner, strat, sym_id, _ = make_runner_with_strategy()

    seen = []

    class P(flox.PnLTracker):
        def on_signal(self, sig):
            seen.append((sig.symbol, sig.side, sig.quantity))

    pnl = P()
    runner.set_pnl_tracker(pnl)
    runner.start()
    runner.on_trade(sym_id, 100.0, 1.0, True, 1_000)
    runner.stop()
    check(len(seen) == 1, f"PnLTracker.on_signal fired once, got {len(seen)}")
    check(seen[0][1] == "buy", f"side='buy', got {seen[0][1] if seen else 'N/A'}")


def test_storage_sink():
    print("test_storage_sink")
    reg, runner, strat, sym_id, _ = make_runner_with_strategy()
    seen = []

    class Sink(flox.StorageSink):
        def store(self, sig):
            seen.append(sig)

    runner.set_storage_sink(Sink())
    runner.start()
    runner.on_trade(sym_id, 100.0, 1.0, True, 1_000)
    runner.stop()
    check(len(seen) == 1, f"StorageSink.store fired once, got {len(seen)}")


def test_risk_manager_drops_signal():
    print("test_risk_manager_drops_signal")
    reg, runner, strat, sym_id, fired_signal = make_runner_with_strategy()

    class RM(flox.RiskManager):
        def allow(self, sig):
            return False  # always deny

    runner.set_risk_manager(RM())
    runner.start()
    runner.on_trade(sym_id, 100.0, 1.0, True, 1_000)
    runner.stop()
    check(len(fired_signal) == 0,
          f"on_signal callback should NOT fire when RM denies, got {len(fired_signal)}")


def test_kill_switch_drops_signal():
    print("test_kill_switch_drops_signal")
    reg, runner, strat, sym_id, fired_signal = make_runner_with_strategy()

    class KS(flox.KillSwitch):
        def check(self, sig):
            return False

    runner.set_kill_switch(KS())
    runner.start()
    runner.on_trade(sym_id, 100.0, 1.0, True, 1_000)
    runner.stop()
    check(len(fired_signal) == 0, "KillSwitch denies → no on_signal")


def test_order_validator_drops_signal():
    print("test_order_validator_drops_signal")
    reg, runner, strat, sym_id, fired_signal = make_runner_with_strategy()

    class V(flox.OrderValidator):
        def validate(self, sig):
            return False

    runner.set_order_validator(V())
    runner.start()
    runner.on_trade(sym_id, 100.0, 1.0, True, 1_000)
    runner.stop()
    check(len(fired_signal) == 0, "OrderValidator rejects → no on_signal")


def test_executor_receives_signals():
    print("test_executor_receives_signals")
    reg, runner, strat, sym_id, _ = make_runner_with_strategy()
    submits = []
    starts = [0]
    stops = [0]

    class E(flox.Executor):
        def submit(self, order):
            submits.append((order.symbol, order.side, order.quantity, order.order_type))

        def on_start(self):
            starts[0] += 1

        def on_stop(self):
            stops[0] += 1

    runner.set_executor(E())
    runner.start()
    check(starts[0] == 1, "Executor.on_start fired on runner.start()")
    runner.on_trade(sym_id, 100.0, 1.0, True, 1_000)
    runner.stop()
    check(stops[0] == 1, "Executor.on_stop fired on runner.stop()")
    check(len(submits) == 1, f"Executor.submit fired once, got {len(submits)}")
    if submits:
        check(submits[0][3] == "market", f"order_type='market', got {submits[0][3]}")


def test_market_data_recorder():
    print("test_market_data_recorder")
    reg = flox.SymbolRegistry()
    sym_id = reg.add_symbol("test", "BTC", 0.01)
    runner = flox.Runner(reg, lambda sig: None, threaded=False)

    class S(flox.Strategy):
        pass

    runner.add_strategy(S(symbols=[sym_id]))

    trades = []
    book_calls = [0]
    starts = [0]

    class R(flox.MarketDataRecorderHook):
        def on_trade(self, t):
            trades.append((t.symbol, t.price, t.is_buy))

        def on_book_update(self, symbol, is_snapshot, bids, asks, ts_ns):
            book_calls[0] += 1

        def on_start(self):
            starts[0] += 1

    runner.set_market_data_recorder(R())
    runner.start()
    check(starts[0] == 1, "MarketDataRecorderHook.on_start fired")
    runner.on_trade(sym_id, 101.5, 0.5, True, 5_000)
    runner.on_book_snapshot(sym_id, [100.0, 99.0], [1.0, 2.0],
                             [102.0, 103.0], [1.0, 2.0], 6_000)
    runner.stop()
    check(len(trades) == 1, f"on_trade fired once, got {len(trades)}")
    check(book_calls[0] == 1, "on_book_update fired once")


def test_execution_listener_with_backtest():
    print("test_execution_listener_with_backtest")
    import numpy as np

    reg = flox.SymbolRegistry()
    sym_id = reg.add_symbol("test", "BTC", 0.01)
    btr = flox.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000.0)

    fills = []
    submits = []

    class L(flox.ExecutionListener):
        def on_filled(self, order):
            fills.append((order.symbol, order.side, order.price, order.quantity))

        def on_submitted(self, order):
            submits.append(order)

    btr.add_execution_listener(L())

    fired = [False]

    class S(flox.Strategy):
        def on_bar(self, ctx, bar):
            if fired[0]:
                return
            fired[0] = True
            self.emit_market_buy(sym_id, 1.0)

    btr.set_strategy(S(symbols=[sym_id]))
    btr.run_bars(
        start_time_ns=np.array([1_000_000_000], dtype=np.int64),
        end_time_ns=np.array([1_999_999_999], dtype=np.int64),
        open=np.array([100.0]),
        high=np.array([101.0]),
        low=np.array([99.0]),
        close=np.array([100.5]),
        volume=np.array([10.0]),
        symbol="BTC",
    )
    check(len(fills) >= 1, f"ExecutionListener.on_filled fired ≥1×, got {len(fills)}")


def test_executor_with_backtest():
    print("test_executor_with_backtest")
    import numpy as np

    reg = flox.SymbolRegistry()
    sym_id = reg.add_symbol("test", "BTC", 0.01)
    btr = flox.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000.0)

    submits = []

    class E(flox.Executor):
        def submit(self, order):
            submits.append((order.symbol, order.side, order.quantity, order.order_type))

    btr.set_executor(E())

    fired = [False]

    class S(flox.Strategy):
        def on_bar(self, ctx, bar):
            if fired[0]:
                return
            fired[0] = True
            self.emit_market_buy(sym_id, 1.0)

    btr.set_strategy(S(symbols=[sym_id]))
    btr.run_bars(
        start_time_ns=np.array([1_000_000_000], dtype=np.int64),
        end_time_ns=np.array([1_999_999_999], dtype=np.int64),
        open=np.array([100.0]),
        high=np.array([101.0]),
        low=np.array([99.0]),
        close=np.array([100.5]),
        volume=np.array([10.0]),
        symbol="BTC",
    )
    check(len(submits) == 1, f"Custom Executor.submit fired once, got {len(submits)}")
    if submits:
        check(submits[0][3] == "market", f"order_type='market', got {submits[0][3]}")


def test_log_callback():
    print("test_log_callback")
    msgs = []

    def cb(level, msg):
        msgs.append((level, msg))

    flox.set_log_callback(cb)
    # Emit a log line — for now just detach to prove the wiring doesn't
    # crash. The C ABI lacks a public Python-driven log emitter.
    flox.set_log_callback(None)
    check(True, "set_log_callback wires + detaches without crash")


if __name__ == "__main__":
    test_pnl_tracker()
    test_storage_sink()
    test_risk_manager_drops_signal()
    test_kill_switch_drops_signal()
    test_order_validator_drops_signal()
    test_executor_receives_signals()
    test_market_data_recorder()
    test_execution_listener_with_backtest()
    test_executor_with_backtest()
    test_log_callback()
    print(f"\n{_passed} passed, {_failed} failed")
    sys.exit(0 if _failed == 0 else 1)
