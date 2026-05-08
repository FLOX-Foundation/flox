# Add latency to a backtest

Per-trade fills in flox work in instant mode by default: an order created at time `T` sees the next observed trade as its fill. That is fine for bar-driven strategies on minute-or-larger timeframes. For market-making, latency arbitrage, and HFT-style work, the gap between event arrival, decision, and round-trip to the exchange is what determines whether a fill happens at all.

Latency models live in the C++ engine and are exposed through every binding (Python, Node, Codon, QuickJS) with the same surface. Each draw covers `feed` (event arrival to engine), `order` (engine submit to exchange), and `fill` (exchange match to engine notification). Phase 1 is the sampling primitive. The user app applies samples to its own timestamps before submitting orders to `SimulatedExecutor`. Phase 2 will plumb the primitive into the engine through `BacktestConfig.latency` so one knob controls every fill path.

## The four models

| Model | Use when |
|---|---|
| `ConstantLatency` | Baseline. A fixed delay per component. Good for "what if my round-trip were always 5ms" experiments. |
| `GaussianLatency` | Symmetric jitter around a mean. Good for stable links with a tight measured standard deviation. |
| `ExponentialLatency` | Heavy right tail. Default for network-bound latency where the histogram is one-sided. |
| `EmpiricalLatency` | Resample with replacement from observed values. Use this when you have a recording of live latencies and want backtest realism that matches the distribution shape, including bimodality. |

Every model implements `feed_delay() / order_delay() / fill_delay()` returning non-negative nanoseconds, plus a `sample()` that bundles all three.

## Quick start

=== "Python"

    ```python
    from flox_py.latency_models import GaussianLatency

    latency = GaussianLatency(
        feed_mean_ns=600_000, feed_stddev_ns=80_000,
        order_mean_ns=1_200_000, order_stddev_ns=200_000,
        fill_mean_ns=600_000, fill_stddev_ns=80_000,
        seed=42,
    )
    s = latency.sample()
    print(s.feed_ns, s.order_ns, s.fill_ns)
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    const latency = new flox.GaussianLatency({
      feedMeanNs: 600_000, feedStddevNs: 80_000,
      orderMeanNs: 1_200_000, orderStddevNs: 200_000,
      fillMeanNs: 600_000, fillStddevNs: 80_000,
      seed: 42,
    });
    const s = latency.sample();
    console.log(s.feedNs, s.orderNs, s.fillNs);
    ```

=== "Codon"

    ```python
    from flox.latency import GaussianLatency

    latency = GaussianLatency(
        feed_mean_ns=600_000.0, feed_stddev_ns=80_000.0,
        order_mean_ns=1_200_000.0, order_stddev_ns=200_000.0,
        fill_mean_ns=600_000.0, fill_stddev_ns=80_000.0,
        seed=42)
    s = latency.sample()
    print(s.feed_ns, s.order_ns, s.fill_ns)
    ```

=== "QuickJS"

    ```javascript
    const latency = new flox.GaussianLatency({
        feedMeanNs: 600000, feedStddevNs: 80000,
        orderMeanNs: 1200000, orderStddevNs: 200000,
        fillMeanNs: 600000, fillStddevNs: 80000,
        seed: 42,
    });
    const s = latency.sample();
    console.log(s.feedNs, s.orderNs, s.fillNs);
    ```

Pass `seed` for reproducible runs. `reset(seed)` replays the same sequence.

## Applying samples in your backtest loop

Phase 1 leaves integration to the user app. Around a `SimulatedExecutor` the pattern is:

=== "Python"

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
        sim.advance_clock(ts_ns + s.feed_ns)
        sim.on_trade_qty(sym_id, price, qty, is_buy)
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    const sim = new flox.SimulatedExecutor();
    const latency = new flox.ExponentialLatency({
      feedMeanNs: 400_000, orderMeanNs: 900_000, fillMeanNs: 400_000, seed: 7,
    });

    function onTrade(tsNs, symId, price, qty, isBuy) {
      const s = latency.sample();
      sim.advanceClock(tsNs + s.feedNs);
      sim.onTradeQty(symId, price, qty, isBuy);
    }
    ```

Same shape in every binding: pull a sample, add the right component to the relevant timestamp, hand the delayed value to the simulator.

## Calibrating from a recording

If you have measured latencies from a live run, hand the arrays to `EmpiricalLatency`:

=== "Python"

    ```python
    from flox_py.latency_models import calibrate_from_samples

    latency = calibrate_from_samples(
        feed_samples=feed_arr,
        order_samples=order_arr,
        fill_samples=fill_arr,
        seed=11,
    )
    ```

=== "Node.js"

    ```javascript
    const latency = new flox.EmpiricalLatency({
      feedSamples: feedArr,
      orderSamples: orderArr,
      fillSamples: fillArr,
      seed: 11,
    });
    ```

Sampling is uniform with replacement. The resulting distribution shape matches the recording exactly, no smoothing or kernel density estimate. For a smoothed distribution, fit a parametric model and use `GaussianLatency` or `ExponentialLatency` instead.

## When to skip latency entirely

For bar-driven strategies on minute-or-larger timeframes, latency rarely changes backtest results. Instant mode is the right default. Reach for this module when:

- You are market-making and round-trip determines whether you get a fill at all.
- You are testing a latency-arbitrage strategy where round-trip is the whole point.
- A live recording diverges from the instant-mode backtest and you want to localize whether the gap is latency-driven.

## What is not here yet (Phase 2)

- Engine-level integration through `BacktestConfig.latency`. Today the user app applies samples manually; Phase 2 wires the primitive into `SimulatedExecutor` so one knob controls every fill path.
- Per-symbol calibration. Phase 1 is global per component.

## See also

- [Backtest with realistic fills](backtest-realistic-fills.md). Slippage and queue position, the companion knobs already wired into `SimulatedExecutor`.
- [Reproducibility bundles](reproducibility-bundles.md). Seed the latency model from the bundle's manifest to make the draws part of the reproducibility contract.
- [Replay-equivalence gate](../explanation/replay-equivalence-gate.md). Seed deterministically and the gate keeps holding even with latency on.
