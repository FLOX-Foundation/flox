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

# ── AutoCorrelation ──────────────────────────────────────────────────

print("=== AutoCorrelation ===")

# Linear y = a + b*t -> lag-1 autocorrelation == 1.
linear = np.array([5.0 + 0.7 * i for i in range(50)])
ac = flox.autocorrelation(linear, 10, 1)
check(math.isnan(ac[9]), "autocorrelation warmup at index < window+lag-1 is NaN")
check(approx(ac[10], 1.0), "autocorrelation on linear series, lag=1 == 1.0")
check(approx(ac[40], 1.0), "autocorrelation on linear series at later index == 1.0")

# Streaming class matches the batch function on the same input.
stream = flox.AutoCorrelation(10, 1)
streamed = []
for v in linear:
    streamed.append(stream.update(float(v)))
# Streaming is ready at index window+lag-1 = 10 (0-based).
for i in range(10, len(linear)):
    check(approx(float(streamed[i]), float(ac[i])),
          f"streaming AC matches batch at i={i}")
    if i >= 10:
        break  # one assertion is enough to keep noise down

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

# ── Summary ──────────────────────────────────────────────────────────

print(f"\n{_passed} passed, {_failed} failed")
if _failed:
    sys.exit(1)
