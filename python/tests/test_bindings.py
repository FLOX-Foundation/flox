"""
python/tests/test_bindings.py — smoke-test all major Python binding surfaces.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_bindings.py
"""

import sys
import os
import math

build_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'python')
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox
import numpy as np

_passed = 0
_failed = 0


def check(condition, msg):
    global _passed, _failed
    if condition:
        print(f"  ok  {msg}")
        _passed += 1
    else:
        print(f"  FAIL  {msg}")
        _failed += 1


def approx(a, b, eps=1e-6):
    return math.isfinite(a) and math.isfinite(b) and abs(a - b) < eps


# ── Constants ────────────────────────────────────────────────────────

print("=== Constants ===")

check(flox.SLIPPAGE_NONE == 0, "SLIPPAGE_NONE == 0")
check(flox.SLIPPAGE_FIXED_TICKS == 1, "SLIPPAGE_FIXED_TICKS == 1")
check(flox.SLIPPAGE_FIXED_BPS == 2, "SLIPPAGE_FIXED_BPS == 2")
check(flox.SLIPPAGE_VOLUME_IMPACT == 3, "SLIPPAGE_VOLUME_IMPACT == 3")
check(flox.QUEUE_NONE == 0, "QUEUE_NONE == 0")
check(flox.QUEUE_TOB == 1, "QUEUE_TOB == 1")
check(flox.QUEUE_FULL == 2, "QUEUE_FULL == 2")

check(flox.PRICE_SCALE == 100_000_000, "PRICE_SCALE == 1e8")
check(flox.QUANTITY_SCALE == 100_000_000, "QUANTITY_SCALE == 1e8")
check(flox.VOLUME_SCALE == 100_000_000, "VOLUME_SCALE == 1e8")

# ── Raw-to-double converters ─────────────────────────────────────────

print("=== Raw-to-double converters ===")

# Contiguous int64 input
contig = np.array([100_00000000, 200_00000000, 300_00000000], dtype=np.int64)
out = flox.prices_to_double(contig)
check(np.allclose(out, [100.0, 200.0, 300.0]), "prices_to_double on contiguous 1D")
check(out.dtype == np.float64, "prices_to_double returns float64")

# Non-contiguous slice (every-other element)
strided_src = np.array([1, 999, 2, 999, 3, 999], dtype=np.int64) * 100_000_000
strided = strided_src[::2]
check(not strided.flags["C_CONTIGUOUS"], "test setup: strided view is non-contiguous")
out = flox.prices_to_double(strided)
check(np.allclose(out, [1.0, 2.0, 3.0]),
      "prices_to_double on non-contiguous strided view (every-other element)")

# Field of a structured array — the canonical buggy case
bar_dtype = np.dtype([
    ("start_time_ns", "<i8"),
    ("end_time_ns", "<i8"),
    ("open_raw", "<i8"),
    ("high_raw", "<i8"),
    ("low_raw", "<i8"),
    ("close_raw", "<i8"),
    ("volume_raw", "<i8"),
    ("buy_volume_raw", "<i8"),
    ("trade_count", "<i8"),
])
bars = np.zeros(5, dtype=bar_dtype)
bars["close_raw"] = np.array([100_00000000, 101_00000000, 102_00000000,
                              103_00000000, 104_00000000], dtype=np.int64)
# Set other fields to garbage to ensure we don't accidentally read them
bars["open_raw"] = 999_00000000
bars["volume_raw"] = 7777_00000000
out = flox.prices_to_double(bars["close_raw"])
check(np.allclose(out, [100.0, 101.0, 102.0, 103.0, 104.0]),
      "prices_to_double on structured-array field view (close_raw)")

out_v = flox.volumes_to_double(bars["volume_raw"])
check(np.allclose(out_v, [7777.0] * 5), "volumes_to_double on structured-array field view")

# 2D input — shape preserved
two_d = np.arange(6, dtype=np.int64).reshape(2, 3) * 100_000_000
out2 = flox.quantities_to_double(two_d)
check(out2.shape == (2, 3), "quantities_to_double preserves 2D shape")
check(np.allclose(out2, [[0.0, 1.0, 2.0], [3.0, 4.0, 5.0]]), "quantities_to_double 2D values")

# ── Streaming indicators ──────────────────────────────────────────────

print("=== Streaming indicators ===")

sma = flox.SMA(3)
sma.update(1.0); sma.update(2.0); sma.update(3.0)
check(approx(sma.value, 2.0), "SMA(3) of [1,2,3] == 2.0")
sma.update(4.0)
check(approx(sma.value, 3.0), "SMA(3) shifts to [2,3,4] == 3.0")

ema = flox.EMA(3)
for v in range(1, 11):
    ema.update(float(v))
check(ema.value > 0, "EMA value > 0 after 10 updates")
check(ema.value > 5.0, "EMA(10 ascending) > 5.0")

rsi = flox.RSI(14)
for v in range(1, 30):
    rsi.update(float(v))
check(0.0 <= rsi.value <= 100.0, f"RSI in [0,100], got {rsi.value:.2f}")

atr = flox.ATR(14)
for i in range(20):
    atr.update(100.0 + i, 99.0 + i, 100.0 + i)
check(atr.value > 0.0, "ATR > 0")

# MACD
macd = flox.MACD(3, 6, 2)
for v in range(1, 20):
    macd.update(float(v))
check(math.isfinite(macd.value), "MACD.value is finite")

# Bollinger
bb = flox.Bollinger(5, 2.0)
for v in [10.0, 11.0, 12.0, 10.0, 11.0, 12.0, 13.0]:
    bb.update(v)
check(math.isfinite(bb.value), "Bollinger.value is finite")

# ── Batch indicators ──────────────────────────────────────────────────

print("=== Batch indicators ===")

prices = np.array([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0])
out = flox.sma(prices, 3)
check(approx(out[2], 2.0), "batch sma[2] == 2.0")
check(approx(out[6], 6.0), "batch sma[6] == 6.0")

out_ema = flox.ema(prices, 3)
check(len(out_ema) == len(prices), "batch ema length matches input")
check(out_ema[6] > out_ema[2], "batch ema is increasing for ascending input")

# ── Targets (forward-looking labels) ──────────────────────────────────

print("=== Targets ===")

close = np.array([100.0, 101.0, 99.0, 105.0, 110.0])
fr = flox.targets.future_return(close, 2)
check(approx(fr[0], 99.0 / 100.0 - 1.0), "future_return[0] == c[2]/c[0]-1")
check(approx(fr[2], 110.0 / 99.0 - 1.0), "future_return[2] == c[4]/c[2]-1")
check(math.isnan(fr[3]) and math.isnan(fr[4]), "future_return tail is NaN")

const_close = np.full(20, 100.0)
vol = flox.targets.future_ctc_volatility(const_close, 5)
check(approx(vol[0], 0.0), "future_ctc_volatility on const series == 0")
check(math.isnan(vol[19]), "future_ctc_volatility tail is NaN")

linear = np.array([100.0 + 0.5 * i for i in range(20)])
sl = flox.targets.future_linear_slope(linear, 4)
check(approx(sl[0], 0.5), "future_linear_slope on linear series == 0.5")
check(math.isnan(sl[19]), "future_linear_slope tail is NaN")

# ── ADF stationarity test ────────────────────────────────────────────

print("=== ADF ===")

rng = np.random.default_rng(42)
walk = np.cumsum(rng.standard_normal(500))
res = flox.adf(walk, max_lag=4, regression="c")
check(res["used_lag"] <= 4, "adf reports used_lag")
check(res["p_value"] > 0.05, "adf does not reject unit root for random walk")

stationary = rng.standard_normal(500) * 0.5
res2 = flox.adf(stationary, max_lag=4, regression="c")
check(res2["test_stat"] < res["test_stat"], "stationary series produces lower test_stat")
check(res2["p_value"] < 0.05, "adf rejects unit root for white noise")
# ── AutoCorrelation ──────────────────────────────────────────────────

print("=== AutoCorrelation ===")

# Linear y = a + b*t -> lag-1 autocorrelation == 1.
ac_linear = np.array([5.0 + 0.7 * i for i in range(50)])
ac = flox.autocorrelation(ac_linear, 10, 1)
check(math.isnan(ac[9]), "autocorrelation warmup at index < window+lag-1 is NaN")
check(approx(ac[10], 1.0), "autocorrelation on linear series, lag=1 == 1.0")
check(approx(ac[40], 1.0), "autocorrelation on linear series at later index == 1.0")

# Streaming class matches the batch function on the same input.
stream = flox.AutoCorrelation(10, 1)
streamed = []
for v in ac_linear:
    streamed.append(stream.update(float(v)))
# Streaming is ready at index window+lag-1 = 10 (0-based).
for i in range(10, len(ac_linear)):
    check(approx(float(streamed[i]), float(ac[i])),
          f"streaming AC matches batch at i={i}")
    if i >= 10:
        break  # one assertion is enough to keep noise down

# ── IndicatorGraph (batch) ───────────────────────────────────────────

print("=== IndicatorGraph (batch) ===")

ramp = np.array([float(i) for i in range(50)])
g = flox.IndicatorGraph()
g.set_bars(0, ramp)

# Two independent batch nodes + one dependent node that reads its parents.
g.add_node("ema5", [], lambda graph, sym: flox.ema(graph.close(sym), 5))
g.add_node("sma5", [], lambda graph, sym: flox.sma(graph.close(sym), 5))
g.add_node("diff", ["ema5", "sma5"],
           lambda graph, sym: graph.get(sym, "ema5") - graph.get(sym, "sma5"))

ema5 = g.require(0, "ema5")
diff = g.require(0, "diff")
check(len(ema5) == 50, "graph require returns full-length array")
check(len(diff) == 50, "graph dependent node has same length")
# get() returns cached value after require ran.
sma5_cached = g.get(0, "sma5")
check(sma5_cached is not None, "get returns the cached node array after require")
check(np.allclose(diff, ema5 - sma5_cached, equal_nan=True), "dependent node uses parents")

# Unknown node → throws.
threw = False
try:
    g.require(0, "missing")
except Exception:
    threw = True
check(threw, "require on unknown node throws")

# ── StreamingIndicatorGraph ───────────────────────────────────────────

print("=== StreamingIndicatorGraph ===")

sg = flox.StreamingIndicatorGraph()
sg.add_node("double_close", [], lambda graph, sym: graph.close(sym) * 2.0)

closes_s = np.array([10.0, 20.0, 30.0, 40.0, 50.0])
for c in closes_s:
    sg.step(0, c)

check(approx(sg.current(0, "double_close"), 100.0), "streaming current after 5 steps")
check(sg.bar_count(0) == 5, "streaming bar_count == 5")

# Parity: batch on same data → last element == streaming current.
bg2 = flox.IndicatorGraph()
bg2.set_bars(0, closes_s)
bg2.add_node("double_close", [], lambda graph, sym: graph.close(sym) * 2.0)
batch_dc = bg2.require(0, "double_close")
check(approx(sg.current(0, "double_close"), batch_dc[-1]),
      "streaming current == batch last element")

sg.reset(0)
check(sg.bar_count(0) == 0, "after reset bar_count == 0")
check(math.isnan(sg.current(0, "double_close")), "after reset current is NaN")

# ── Discovery API ────────────────────────────────────────────────────

print("=== Discovery ===")

names = flox.list_indicators()
check(len(names) >= 21, f"list_indicators returns at least 21 entries (got {len(names)})")
check("EMA" in names, "EMA in list_indicators")
check("MACD" in names, "MACD in list_indicators")
check("AutoCorrelation" in names, "AutoCorrelation in list_indicators")

# ── Declarative DAG helper: g.indicator(name, factory, source=) ─────

print("=== g.indicator helper ===")

g3 = flox.IndicatorGraph()
prices_d = np.array([10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0])
g3.set_bars(0, prices_d)
g3.indicator("ema3", flox.EMA(3), source="close")
ema_arr = g3.require(0, "ema3")
check(len(ema_arr) == len(prices_d), "g.indicator(EMA(3)) returns full-length array")
check(np.isnan(ema_arr[0]) and np.isnan(ema_arr[1]), "g.indicator EMA warmup is NaN")
check(approx(float(ema_arr[2]), 20.0), "g.indicator EMA(3) seed at index 2 == 20.0")

# ── Unified streaming on the same IndicatorGraph (no separate type) ─

print("=== Unified DAG: step+current on IndicatorGraph ===")

g_unified = flox.IndicatorGraph()
g_unified.add_node("triple_close", [], lambda gr, sym: gr.close(sym) * 3.0)
for v in [1.0, 2.0, 3.0]:
    g_unified.step(0, v)
check(approx(g_unified.current(0, "triple_close"), 9.0),
      "IndicatorGraph.step+current on the same instance (was StreamingIndicatorGraph)")
check(g_unified.bar_count(0) == 3, "IndicatorGraph.bar_count works on same instance")

# Backward-compat alias: flox.StreamingIndicatorGraph is the same class.
check(flox.StreamingIndicatorGraph is flox.IndicatorGraph,
      "flox.StreamingIndicatorGraph aliases flox.IndicatorGraph")

# ── PositionTracker ───────────────────────────────────────────────────

print("=== PositionTracker ===")

pt = flox.PositionTracker("fifo")
pt.on_fill(0, "buy", 50000.0, 1.0)
check(approx(pt.position(0), 1.0), "position == 1.0 after buy")
check(approx(pt.avg_entry_price(0), 50000.0), "avg_entry_price == 50000")
check(approx(pt.realized_pnl(0), 0.0), "realized_pnl == 0 before close")

pt.on_fill(0, "sell", 51000.0, 1.0)
check(approx(pt.position(0), 0.0), "position == 0 after sell")
check(pt.realized_pnl(0) > 0.0, "realized_pnl > 0 on winning trade")
check(approx(pt.realized_pnl(0), 1000.0), "realized_pnl == 1000")
check(approx(pt.total_realized_pnl(), 1000.0), "total_realized_pnl == 1000")

# Average cost method
pt_avg = flox.PositionTracker("average")
pt_avg.on_fill(0, "buy", 100.0, 2.0)
pt_avg.on_fill(0, "buy", 200.0, 2.0)
check(approx(pt_avg.position(0), 4.0), "avg cost: position == 4.0")
check(approx(pt_avg.avg_entry_price(0), 150.0), "avg cost: avg_entry == 150.0")

# Multi-symbol
pt2 = flox.PositionTracker("fifo")
pt2.on_fill(0, "buy", 100.0, 2.0)
pt2.on_fill(1, "sell", 200.0, 3.0)
check(approx(pt2.position(0), 2.0), "symbol 0 position == 2.0")
check(approx(pt2.position(1), -3.0), "symbol 1 position == -3.0")

# ── OrderTracker ─────────────────────────────────────────────────────

print("=== OrderTracker ===")

ot = flox.OrderTracker()
check(ot.on_submitted(1001, "EX-001") == True, "on_submitted returns True")
check(ot.is_active(1001), "order 1001 is active")
check(ot.active_count() == 1, "active_count == 1")
check(ot.total_count() == 1, "total_count == 1")

ot.on_submitted(1002, "EX-002")
check(ot.active_count() == 2, "active_count == 2 after second order")

check(ot.on_filled(1001, 1.0) == True, "on_filled returns True")
check(not ot.is_active(1001), "order 1001 inactive after fill")
check(ot.active_count() == 1, "active_count == 1 after fill")

check(ot.on_canceled(1002) == True, "on_canceled returns True")
check(not ot.is_active(1002), "order 1002 inactive after cancel")
check(ot.active_count() == 0, "active_count == 0")
check(ot.total_count() == 2, "total_count == 2")

# ── Engine (data loading) ────────────────────────────────────────────

print("=== Engine ===")

csv_path = os.path.abspath(
    os.path.join(os.path.dirname(__file__), '..', '..', 'docs', 'examples', 'data', 'btcusdt_1m.csv')
)
if os.path.exists(csv_path):
    engine = flox.Engine()
    engine.load_csv(csv_path)
    n = engine.bar_count()
    check(n > 1000, f"bar_count {n} > 1000")
    opens = engine.open()
    closes = engine.close()
    highs = engine.high()
    lows = engine.low()
    check(len(opens) == n, "len(opens) == bar_count")
    check(len(closes) == n, "len(closes) == bar_count")
    check(all(opens[:10] > 0), "first 10 open prices > 0")
    check(all(highs >= lows), "high >= low for every bar")
    print(f"  loaded {n} bars, price {min(closes):.0f}–{max(closes):.0f}")
else:
    print(f"  skip Engine (CSV not found: {csv_path})")

# ── FloxError ────────────────────────────────────────────────────────

print("=== FloxError ===")

# The FloxError class is exposed at the top level and inherits from Exception
# so existing `except Exception` blocks still catch it.
check(issubclass(flox.FloxError, Exception),
      "FloxError is an Exception subclass")

# Triggering an "unknown symbol" path via Engine.run on a registry that
# never saw the requested symbol. We need data to be loaded and the
# strategy to reference a symbol not in the engine's set. Without a CSV
# fixture we exercise the code via a SignalBuilder + run path that the
# engine itself rejects.
if os.path.exists(csv_path):
    engine = flox.Engine()
    engine.load_csv(csv_path)
    sb = flox.SignalBuilder()
    sb.buy(0, 1.0, symbol="ETH/USDT-not-registered")
    try:
        engine.run(sb)
    except flox.FloxError as e:
        check(e.code == "E_SYM_001",
              f'FloxError.code == "E_SYM_001" (got {e.code!r})')
        check("not registered" in e.message,
              f'FloxError.message mentions "not registered"')
        check(e.help_url.endswith("E_SYM_001/"),
              f'FloxError.help_url ends with "E_SYM_001/"')
        check(e.help_url.startswith("https://"),
              f'FloxError.help_url is an https URL')
    else:
        check(False, "engine.run did not raise on unknown symbol")
else:
    print(f"  skip FloxError (CSV not found: {csv_path})")

# ── BacktestRunner accessors ─────────────────────────────────────────

print("=== BacktestRunner equity_curve / trades ===")

bt_csv = os.path.join(os.path.dirname(__file__), '..',
                      'flox_py', 'templates', 'research',
                      'data', 'btcusdt_sample.csv')
if os.path.exists(bt_csv):
    reg2 = flox.SymbolRegistry()
    btc2 = reg2.add_symbol("exchange", "BTCUSDT", 0.01)

    class _SMA(flox.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.fast = flox.SMA(10)
            self.slow = flox.SMA(30)

        def on_trade(self, ctx, t):
            f = self.fast.update(t.price)
            s = self.slow.update(t.price)
            if f is None or s is None:
                return
            if f > s and ctx.is_flat():
                self.market_buy(0.01)
            elif f < s and ctx.is_flat():
                self.market_sell(0.01)

    bt = flox.BacktestRunner(reg2, fee_rate=0.0004, initial_capital=10_000)
    bt.set_strategy(_SMA([btc2]))
    stats = bt.run_csv(bt_csv, symbol="BTCUSDT")
    check(stats["total_trades"] > 0,
          f"BacktestRunner produced trades (got {stats['total_trades']})")

    eq = bt.equity_curve()
    check("timestamp_ns" in eq and "equity" in eq and "drawdown_pct" in eq,
          "equity_curve() returns dict with expected keys")
    check(len(eq["equity"]) == stats["total_trades"],
          f"equity points = total_trades (got {len(eq['equity'])} vs "
          f"{stats['total_trades']})")
    check(eq["equity"].dtype == np.float64,
          "equity is float64 numpy array")

    tr = bt.trades()
    expected = ["symbol", "side", "entry_price", "exit_price",
                "quantity", "pnl", "fee", "entry_time_ns", "exit_time_ns"]
    check(all(k in tr for k in expected),
          f"trades() returns dict with all expected keys")
    check(len(tr["pnl"]) == stats["total_trades"],
          f"trades count = total_trades (got {len(tr['pnl'])} vs "
          f"{stats['total_trades']})")

    fresh = flox.BacktestRunner(reg2, fee_rate=0.0004, initial_capital=10_000)
    try:
        fresh.equity_curve()
        check(False, "equity_curve() before run should raise")
    except flox.FloxError as e:
        check(e.code == "E_RUN_002",
              f"equity_curve before run → FloxError E_RUN_002 (got {e.code!r})")
else:
    print(f"  skip BacktestRunner accessors (CSV not found: {bt_csv})")

# ── Summary ──────────────────────────────────────────────────────────

print(f"\n{_passed} passed, {_failed} failed")
if _failed:
    sys.exit(1)
