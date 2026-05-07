# Add latency to a backtest

Per-trade fills in flox today work in instant mode: an order created at time `T` sees the next observed trade as its fill. That is fine for bar-driven strategies on minute-or-larger timeframes. It is unrealistic for market-making, latency arbitrage, and any HFT-style work where the gap between event arrival, decision, and round-trip to the exchange determines whether a fill happens at all.

`flox_py.latency_models` exposes a small `LatencyModel` interface plus four implementations, each sampling three independent components: `feed` (event arrival to engine), `order` (engine submit to exchange), and `fill` (exchange match to engine notification). Phase 1 is the sampling primitive itself; the user app applies samples to its own observed-trade and order-submit timestamps before feeding them through `flox_py.SimulatedExecutor`. Phase 2 will plumb the same primitive into the engine automatically through `BacktestConfig.latency`.

## The four models

| Model | Use when |
|---|---|
| `ConstantLatency(feed_ns, order_ns, fill_ns)` | Baseline. Adds a fixed delay to each component. Useful for "what if my round-trip were always 5ms" experiments. |
| `GaussianLatency(feed_mean, feed_stddev, ...)` | Symmetric jitter around a mean. Good for stable links where you measured a tight standard deviation. |
| `ExponentialLatency(feed_mean, order_mean, fill_mean)` | Heavy right tail. The default for network-bound latency where the histogram is one-sided. |
| `EmpiricalLatency(feed_samples, order_samples, fill_samples)` | Resample with replacement from observed values. The right model when you have a recording of live latencies and want backtest realism that matches the exact distribution shape, including bimodality. |

Every model implements `feed_delay() / order_delay() / fill_delay() -> int` (nanoseconds) and a convenience `sample() -> LatencySample`. All return non-negative integers; negative samples are clamped to zero.

## Quick start

```python
from flox_py.latency_models import GaussianLatency

# Cable round-trip ~ 1.2 ms with 200 us jitter:
latency = GaussianLatency(
    feed_mean_ns=600_000, feed_stddev_ns=80_000,
    order_mean_ns=1_200_000, order_stddev_ns=200_000,
    fill_mean_ns=600_000, fill_stddev_ns=80_000,
    seed=42,  # reproducible sampling
)

s = latency.sample()
print(s.to_dict())
# {'feed_ns': 643812, 'order_ns': 1228104, 'fill_ns': 612944}
```

Pass `seed=...` whenever you need reproducible runs. The same seed produces the same sequence; pair this with the replay-equivalence gate to land latency-aware backtests in CI.

## Applying samples in your backtest loop

Phase 1 leaves integration to the user app. Typical pattern around a `flox_py.SimulatedExecutor`:

```python
import flox_py as flox
from flox_py.latency_models import ExponentialLatency

sim = flox.SimulatedExecutor()
latency = ExponentialLatency(
    feed_mean_ns=400_000,
    order_mean_ns=900_000,
    fill_mean_ns=400_000,
    seed=7,
)

def on_trade(ts_ns, sym_id, price, qty, is_buy):
    s = latency.sample()
    # Feed delay shifts when the engine sees the trade.
    sim.advance_clock(ts_ns + s.feed_ns)
    sim.on_trade_qty(sym_id, price, qty, is_buy)

def on_signal(sig):
    s = latency.sample()
    # Order delay shifts when the simulator considers the order live.
    submit_at = sim_now_ns + s.order_ns
    schedule_submit(submit_at, sig)
```

The exact integration depends on how your loop is structured. The pattern is the same: pull a sample, add to the relevant timestamp, feed the simulator the delayed value.

## Calibrating from a recording

If you have measured latencies from a live run, wrap them with `calibrate_from_samples`:

```python
from flox_py.latency_models import calibrate_from_samples

latency = calibrate_from_samples(
    feed_samples=feed_arr,    # list of ints (ns) from your recording
    order_samples=order_arr,
    fill_samples=fill_arr,
    seed=11,
)
```

Sampling is uniform with replacement, so the resulting distribution shape matches the recording byte-for-byte (no smoothing, no kernel density estimate). If you want a smoothed distribution, fit a parametric model to the samples and use `GaussianLatency` or `ExponentialLatency` instead.

## When to skip latency entirely

For bar-driven strategies on minute-or-larger timeframes, latency rarely affects backtest results. Instant mode is the right default. Pull this module in when:

- You are doing market making and round-trip determines whether you even get a fill.
- You are testing a latency-arbitrage strategy where the dependency on round-trip is the entire point.
- You have a live recording that diverges from the instant-mode backtest and you want to localize whether the divergence is latency-driven.

## What's not here yet (Phase 2)

- Engine-level integration through `BacktestConfig.latency`. Today the user app applies samples manually; Phase 2 will plumb the same primitive into `SimulatedExecutor` so a single config knob controls every fill path.
- Cross-binding parity. The C++ and Node / Codon / QuickJS sides land in the same Phase 2.
- Per-symbol calibration. Phase 1 is global per-component; per-symbol latency lives in Phase 2.

## See also

* [Backtest with realistic fills](backtest-realistic-fills.md). Slippage and queue position; the companion knobs that already plug into `SimulatedExecutor`.
* [Reproducibility bundles](reproducibility-bundles.md). Seed your latency model from the bundle's manifest to make the latency draws part of the reproducibility contract.
* [Replay-equivalence gate](../explanation/replay-equivalence-gate.md). Latency-aware backtests should still be reproducible; seed the model deterministically and the gate keeps holding.
