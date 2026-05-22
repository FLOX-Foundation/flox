# Model perpetual funding payments

Perpetuals settle funding on a fixed schedule — every 8 hours on
Binance UM / Bybit linear / OKX swap, every hour on some Bitget
markets. The payment per settlement is:

```
amount = -position_signed * mark_price * rate
```

Sign convention: a long (positive position) pays out when rate is
positive (amount negative). A short receives in the same regime.
Negative rate flips both signs.

`FundingSchedule` walks the schedule and produces a `FundingPayment`
record per (settlement timestamp, symbol). The strategy / backtest
runner is responsible for routing the amount into equity, since
flox's BacktestResult is fill-driven and does not own a
periodic-event clock of its own.

## Construct a schedule

=== "Python"

    ```python
    --8<-- "examples/python_funding_schedule.py"
    ```

=== "Node.js"

    ```javascript
    const flox = require('flox-node');
    const sched = new flox.FundingSchedule();
    sched.loadProfile('binance_um_futures');
    sched.setConstantRate(0.0001);
    const events = sched.tick(nowNs, [1, 2], [1.0, -2.0], [50000, 3000]);
    ```

=== "Codon"

    ```python
    from flox.funding_schedule import FundingSchedule

    s = FundingSchedule()
    s.load_profile("binance_um_futures")
    s.set_constant_rate(0.0001)
    events = s.tick(9 * 3600 * 1_000_000_000,
                    [1, 2], [1.0, -2.0], [50000.0, 3000.0])
    ```

=== "QuickJS"

    ```javascript
    const h = __flox_funding_schedule_create();
    __flox_funding_schedule_load_profile(h, "binance_um_futures");
    __flox_funding_schedule_set_constant_rate(h, 0.0001);
    const events = __flox_funding_schedule_tick(h, nowNs, [1, 2],
                                                 [1.0, -2.0],
                                                 [50000.0, 3000.0]);
    ```

=== "C++"

    ```cpp
    auto s = flox::FundingSchedule::binance_um_futures();
    s.setConstantRate(0.0001);
    auto events = s.tick(nowNs, {1, 2}, {1.0, -2.0}, {50000.0, 3000.0});
    ```

## Canned profiles

| Profile               | Interval | Default rate |
|-----------------------|----------|--------------|
| `binance_um_futures`  | 8h       | 0            |
| `bybit_linear`        | 8h       | 0            |
| `okx_swap`            | 8h       | 0            |
| `bitget_hourly`       | 1h       | 0            |

All profiles default to a zero rate — callers must supply a real
rate either via `set_constant_rate` or by attaching a recorded
tape via `tape(...)`.

## Tape mode

For research that wants the exchange's actually-published rates
(not a flat assumption), use `FundingSchedule.tape([(ts, rate),
...])`. Each tape event triggers one payment at its timestamp.

## Integration recipe

A typical backtest tick loop:

```python
last_ns = 0
for event in market_data:
    pos, mark = current_position(event.symbol), event.mark_price
    payments = sched.tick(event.timestamp_ns, [event.symbol], [pos], [mark])
    for p in payments:
        equity += p.amount
        ledger.append(p)
    last_ns = event.timestamp_ns
```

## Notes

- The schedule's internal cursor (`last_tick_ns`) advances on every
  tick. To restart a backtest, call `reset()`.
- Per-day settlement venues (BitMEX-style daily funding) need a
  custom interval — pass `24 * 3600 * 1_000_000_000` to
  `set_constant`.
- The signed position should reflect the actual exposure at the
  funding boundary, not the average over the period. Snap the
  position at the boundary timestamp.
- For multi-symbol portfolios pass parallel arrays — one tick call
  walks every symbol per boundary.
