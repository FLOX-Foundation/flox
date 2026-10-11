# Add latency to a backtest

A backtest runs in instant mode by default: no order latency. On tick and book replay an order is tried the moment it is submitted, against the market state the strategy is reacting to, so a market order sent from `on_trade` takes the book standing at that moment, or that trade's own price when the tape carries no book, not the next print. On bar replay an order sent from the bar callback is held and fills at that symbol's next bar open, because the simulator has already walked the bar the strategy was shown (see [Bar callbacks fill at the next open](../reference/api/backtest/backtest_runner.md#bar-callbacks-fill-at-the-next-open)). That is fine for bar-driven strategies on minute-or-larger timeframes. For market-making, latency arbitrage, and HFT-style work, the gap between event arrival, decision, and round-trip to the exchange is what determines whether a fill happens at all.

Latency models live in the C++ engine and are exposed through every binding (Python, Node, Codon, QuickJS) with the same surface. Each draw covers `feed` (event arrival to engine), `order` (engine submit to exchange), and `fill` (exchange match to engine notification).

In C++, `BacktestConfig::latency` takes one of these models and the simulator applies its `order_delay()` itself: an order submitted at `T` is not marketable until `T + order_delay()`, and its fill is stamped at the time it actually matched. `feed_delay()` and `fill_delay()` are still a sampling primitive the user app applies to its own timestamps -- see [What is wired, what is not](#what-is-wired-what-is-not). For ack latency the simulator has always applied, see the `SimulatedExecutor` setters at the bottom of this page.

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
    from lrvx.latency_models import GaussianLatency

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
    const lrvx = require('@lrvx/lrvx');

    const latency = new lrvx.GaussianLatency({
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
    from lrvx.latency import GaussianLatency

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
    const latency = new lrvx.GaussianLatency({
        feedMeanNs: 600000, feedStddevNs: 80000,
        orderMeanNs: 1200000, orderStddevNs: 200000,
        fillMeanNs: 600000, fillStddevNs: 80000,
        seed: 42,
    });
    const s = latency.sample();
    console.log(s.feedNs, s.orderNs, s.fillNs);
    ```

Pass `seed` for reproducible runs. `reset(seed)` replays the same sequence.

## Order latency the engine applies for you

Attach a model to the run configuration and the executor holds each order out
of matching for the delay it draws:

```cpp
BacktestConfig cfg;
cfg.latency = std::make_shared<ConstantLatency>(/*feed_ns=*/0,
                                                /*order_ns=*/5'000'000,
                                                /*fill_ns=*/0);

SimulatedExecutor exec(clock);
exec.applyConfig(cfg);           // or exec.setLatencyModel(model) directly

exec.submitOrder(marketBuy);     // SUBMITTED fires; nothing fills yet
clock.advanceTo(submitNs + 5'000'000);
exec.onBookUpdate(...);          // ACCEPTED, then the fill, stamped here
```

The delay is drawn once per order and added to the venue's own
`submit_ack_latency_ns`, so a model and an ack profile compose rather than
override each other. The branch is on the sampled value, not on whether a
model is attached: a `ConstantLatency(0, 0, 0)` reproduces the instant
baseline to the nanosecond.

The market is free to move inside the window, which is the point -- the order
fills against whatever book is standing when it arrives, not the one the
strategy saw. A stochastic model keeps drawing from where it left off across
a re-run; call `model.reset(seed)` before the second run to replay the same
sequence.

## Applying feed and fill samples in your backtest loop

The other two components are left to the user app. Around a
`SimulatedExecutor` the pattern is:

=== "Python"

    ```python
    import lrvx
    from lrvx.latency_models import ExponentialLatency

    sim = lrvx.SimulatedExecutor()
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
    const lrvx = require('@lrvx/lrvx');

    const sim = new lrvx.SimulatedExecutor();
    const latency = new lrvx.ExponentialLatency({
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
    from lrvx.latency_models import calibrate_from_samples

    latency = calibrate_from_samples(
        feed_samples=feed_arr,
        order_samples=order_arr,
        fill_samples=fill_arr,
        seed=11,
    )
    ```

=== "Node.js"

    ```javascript
    const latency = new lrvx.EmpiricalLatency({
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

## What is wired, what is not

| Component | Applied by | Effect |
|---|---|---|
| `order_delay()` | The engine, from `BacktestConfig::latency` | The order is not marketable until the delay has elapsed; the fill is stamped when it matched |
| `feed_delay()` | Your app | Add it to the event timestamp before handing the event to the simulator |
| `fill_delay()` | Your app | The simulator dispatches the fill callback as soon as the order matches |

Still open:

- `BacktestConfig::latency` is a C++ field. The Python, Node and QuickJS
  runners take a fee rate and an initial capital rather than a
  `BacktestConfig`, and there is no C API call to attach a model to a
  simulated executor, so from a binding the order delay is not reachable yet.
- Per-symbol calibration. The models are global per component.

`SimulatedExecutor` does have its own ack-latency knobs, independent of the models above, and they *are* wired into the fill path:

| Setter | Effect |
|---|---|
| `set_submit_ack_latency(latency_ns, jitter_ns=0)` | Defers `ACCEPTED` after submit |
| `set_cancel_ack_latency(latency_ns, jitter_ns=0)` | Defers `CANCELED`; the order can still fill in the window |
| `set_replace_ack_latency(latency_ns, jitter_ns=0)` | Defers the replace ack |
| `set_submit_ack_latency_distribution(dist)` | Same, driven by a `LatencyDistribution` |
| `set_cancel_ack_latency_distribution(dist)` | Same |
| `set_replace_ack_latency_distribution(dist)` | Same |
| `apply_latency_profile(name)` | Sets all three from a named venue profile |

`LatencyDistribution` (Node: `new lrvx.LatencyDistribution()` plus `setConstant` / `setUniform` / `setLognormal` / `setEmpirical` / `setBurstCorrelation`) is a separate type from the `*Latency` models above. See [Model cancellation ack latency](cancel-latency-modeling.md).

## See also

- [Backtest with realistic fills](backtest-realistic-fills.md). Slippage and queue position, the companion knobs already wired into `SimulatedExecutor`.
- [Reproducibility bundles](reproducibility-bundles.md). Seed the latency model from the bundle's manifest to make the draws part of the reproducibility contract.
- [Replay-equivalence gate](../explanation/replay-equivalence-gate.md). Pins the instant-mode fills of four order scenarios; it attaches no latency model.
