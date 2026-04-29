"""
python/tests/test_parity.py — batch ↔ streaming parity tests.

For every indicator exposed both as a batch function and a streaming class,
generate a fixed-seed random input, run both paths, and assert the outputs
agree within tolerance.  Failures are blocking CI.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_parity.py
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

ATOL = 1e-9
RTOL = 1e-9


def check(condition, msg):
    global _passed, _failed
    if condition:
        print(f"  ok  {msg}")
        _passed += 1
    else:
        print(f"  FAIL  {msg}")
        _failed += 1


def parity_allclose(batch, streaming, name):
    """Assert batch and streaming outputs agree on non-NaN positions."""
    b = np.asarray(batch, dtype=np.float64)
    s = np.asarray(streaming, dtype=np.float64)
    check(len(b) == len(s), f"{name}: length match ({len(b)} vs {len(s)})")
    nan_b = np.isnan(b)
    nan_s = np.isnan(s)
    check(np.array_equal(nan_b, nan_s), f"{name}: NaN mask match")
    valid = ~nan_b & ~nan_s
    if valid.any():
        close = np.allclose(b[valid], s[valid], atol=ATOL, rtol=RTOL)
        check(close, f"{name}: values agree (atol={ATOL}, rtol={RTOL})")
        if not close:
            worst = np.max(np.abs(b[valid] - s[valid]))
            print(f"         max abs diff = {worst:.3e}")


def rng_prices(n=50, seed=42, lo=100.0, hi=200.0):
    rng = np.random.default_rng(seed)
    return rng.uniform(lo, hi, n)


def rng_hlc(n=50, seed=42):
    """Returns high, low, close arrays with high >= close >= low > 0."""
    rng = np.random.default_rng(seed)
    mid = rng.uniform(100.0, 200.0, n)
    spread = rng.uniform(0.5, 5.0, n)
    high = mid + spread
    low = mid - spread
    close = mid + rng.uniform(-spread, spread, n)
    close = np.clip(close, low, high)
    return high, low, close


def rng_ohlc(n=50, seed=42):
    rng = np.random.default_rng(seed)
    mid = rng.uniform(100.0, 200.0, n)
    spread = rng.uniform(0.5, 5.0, n)
    high = mid + spread
    low = mid - spread
    close = mid + rng.uniform(-spread * 0.9, spread * 0.9, n)
    open_ = mid + rng.uniform(-spread * 0.9, spread * 0.9, n)
    close = np.clip(close, low, high)
    open_ = np.clip(open_, low, high)
    return open_, high, low, close


def streaming_collect(ind, updates):
    """Drive a single-value streaming indicator; return array with NaN for not-ready."""
    out = []
    for v in updates:
        val = ind.update(v)
        out.append(float(val) if val is not None else math.nan)
    return np.array(out)


def streaming_collect_hlc(ind, high, low, close):
    out = []
    for h, l, c in zip(high, low, close):
        val = ind.update(h, l, c)
        out.append(float(val) if val is not None else math.nan)
    return np.array(out)


# ── Single-series indicators ──────────────────────────────────────────

print("=== SMA parity ===")
prices = rng_prices()
period = 10
batch = flox.sma(prices, period)
stream = streaming_collect(flox.SMA(period), prices)
parity_allclose(batch, stream, "SMA")

print("=== EMA parity ===")
batch = flox.ema(prices, period)
stream = streaming_collect(flox.EMA(period), prices)
parity_allclose(batch, stream, "EMA")

print("=== RMA parity ===")
batch = flox.rma(prices, period)
stream = streaming_collect(flox.RMA(period), prices)
parity_allclose(batch, stream, "RMA")

print("=== RSI parity ===")
batch = flox.rsi(prices, period)
stream = streaming_collect(flox.RSI(period), prices)
parity_allclose(batch, stream, "RSI")

print("=== DEMA parity ===")
batch = flox.dema(prices, period)
stream = streaming_collect(flox.DEMA(period), prices)
parity_allclose(batch, stream, "DEMA")

print("=== TEMA parity ===")
batch = flox.tema(prices, period)
stream = streaming_collect(flox.TEMA(period), prices)
parity_allclose(batch, stream, "TEMA")

print("=== KAMA parity ===")
batch = flox.kama(prices, period)
stream = streaming_collect(flox.KAMA(period), prices)
parity_allclose(batch, stream, "KAMA")

print("=== Slope parity ===")
batch = flox.slope(prices, period)
stream = streaming_collect(flox.Slope(period), prices)
parity_allclose(batch, stream, "Slope")

print("=== Skewness parity ===")
batch = flox.skewness(prices, period)
stream = streaming_collect(flox.Skewness(period), prices)
parity_allclose(batch, stream, "Skewness")

print("=== Kurtosis parity ===")
batch = flox.kurtosis(prices, period)
stream = streaming_collect(flox.Kurtosis(period), prices)
parity_allclose(batch, stream, "Kurtosis")

print("=== RollingZScore parity ===")
batch = flox.rolling_zscore(prices, period)
stream = streaming_collect(flox.RollingZScore(period), prices)
parity_allclose(batch, stream, "RollingZScore")

print("=== ShannonEntropy parity ===")
bins = 10
batch = flox.shannon_entropy(prices, period, bins)
stream = streaming_collect(flox.ShannonEntropy(period, bins), prices)
parity_allclose(batch, stream, "ShannonEntropy")

# ── Multi-series: high/low/close ──────────────────────────────────────

high, low, close = rng_hlc()

print("=== ATR parity ===")
batch = flox.atr(high, low, close, period)
stream = streaming_collect_hlc(flox.ATR(period), high, low, close)
parity_allclose(batch, stream, "ATR")

print("=== Stochastic parity ===")
k_period, d_period = 14, 3
b_stoch = flox.stochastic(high, low, close, k_period=k_period, d_period=d_period)
batch_k = b_stoch["k"]
s_stream = []
st = flox.Stochastic(k_period, d_period)
for h, l, c in zip(high, low, close):
    val = st.update(h, l, c)
    s_stream.append(float(val) if val is not None else math.nan)
parity_allclose(batch_k, np.array(s_stream), "Stochastic/k")

print("=== CCI parity ===")
batch = flox.cci(high, low, close, period)
stream = streaming_collect_hlc(flox.CCI(period), high, low, close)
parity_allclose(batch, stream, "CCI")

print("=== ParkinsonVol parity ===")
batch = flox.parkinson_vol(high, low, period)
out = []
pv = flox.ParkinsonVol(period)
for h, l in zip(high, low):
    val = pv.update(h, l)
    out.append(float(val) if val is not None else math.nan)
parity_allclose(batch, np.array(out), "ParkinsonVol")

print("=== RogersSatchellVol parity ===")
open_, high2, low2, close2 = rng_ohlc()
batch = flox.rogers_satchell_vol(open_, high2, low2, close2, period)
out = []
rsv = flox.RogersSatchellVol(period)
for o, h, l, c in zip(open_, high2, low2, close2):
    val = rsv.update(o, h, l, c)
    out.append(float(val) if val is not None else math.nan)
parity_allclose(batch, np.array(out), "RogersSatchellVol")

# ── MACD ─────────────────────────────────────────────────────────────

print("=== MACD parity ===")
fast, slow, signal = 5, 10, 4
b_macd = flox.macd(prices, fast=fast, slow=slow, signal=signal)
batch_line = b_macd["line"]
out = []
m = flox.MACD(fast, slow, signal)
for v in prices:
    val = m.update(v)
    out.append(float(val) if val is not None else math.nan)
parity_allclose(batch_line, np.array(out), "MACD/line")

# ── Bollinger ─────────────────────────────────────────────────────────

print("=== Bollinger parity ===")
b_bol = flox.bollinger(prices, period=period, stddev=2.0)
batch_mid = b_bol["middle"]
out = []
bb = flox.Bollinger(period, 2.0)
for v in prices:
    val = bb.update(v)
    out.append(float(val) if val is not None else math.nan)
parity_allclose(batch_mid, np.array(out), "Bollinger/middle")

# ── Correlation ──────────────────────────────────────────────────────

print("=== Correlation parity ===")
rng = np.random.default_rng(99)
x = rng.uniform(0.0, 1.0, 50)
y = rng.uniform(0.0, 1.0, 50)
batch = flox.correlation(x, y, period)
out = []
cor = flox.Correlation(period)
for xi, yi in zip(x, y):
    val = cor.update(xi, yi)
    out.append(float(val) if val is not None else math.nan)
parity_allclose(batch, np.array(out), "Correlation")

# ── IndicatorGraph batch vs streaming ────────────────────────────────

print("=== IndicatorGraph streaming parity ===")
prices_sg = rng_prices(n=30)
high_sg, low_sg, close_sg = rng_hlc(n=30)

# Batch graph
bg = flox.IndicatorGraph()
bg.add_node("close", [], lambda g, sym: g.close(sym))
bg.add_node("dema5", ["close"], lambda g, sym: flox.dema(np.array(g.require(sym, "close")), 5))
bg.set_bars(0, close_sg, high_sg, low_sg)
batch_dema = list(bg.require(0, "dema5"))

# Streaming graph
sg = flox.StreamingIndicatorGraph()
sg.add_node("close", [], lambda g, sym: g.close(sym))
sg.add_node("dema5", ["close"], lambda g, sym: flox.dema(np.array(g.require(sym, "close")), 5))
for i, (c, h, l) in enumerate(zip(close_sg, high_sg, low_sg)):
    sg.step(0, c, h, l)
stream_dema_last = sg.current(0, "dema5")
check(
    not math.isnan(stream_dema_last) and abs(stream_dema_last - batch_dema[-1]) < ATOL,
    f"IndicatorGraph streaming last == batch last (diff={abs(stream_dema_last - batch_dema[-1]):.2e})"
)
check(sg.bar_count(0) == len(close_sg), "IndicatorGraph streaming bar_count matches input")

# ── Summary ──────────────────────────────────────────────────────────

print(f"\n{_passed} passed, {_failed} failed")
if _failed:
    sys.exit(1)
