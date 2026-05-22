# Model venue volume-tiered maker / taker fees

A flat fee rate buries a real PnL driver. For a market-maker
running $10M+ daily volume, the difference between VIP-0 and VIP-3
is the entire margin. Strategies that look profitable at VIP-0 may
be net-negative once the rebate ladder kicks in, and vice-versa.

`FeeSchedule` ships a volume-tiered ladder that mirrors how real
venues bill: add tiers with `(min 30-day notional, maker bps, taker
bps)`, record every fill, and the schedule resolves the active tier
from the 30-day rolling sum on each lookup.

## Configure

=== "Python"

    ```python
    --8<-- "examples/python_fee_schedule.py"
    ```

=== "Node.js"

    ```javascript
    const flox = require('flox-node');
    const sched = new flox.FeeSchedule();
    sched.loadProfile('binance_um_futures');
    sched.recordFill(tsNs, notional);
    const fee = sched.feeFor(tsNs, notional, /*isMaker=*/false);
    ```

=== "Codon"

    ```python
    from flox.fee_schedule import FeeSchedule

    s = FeeSchedule()
    s.load_profile("binance_um_futures")
    s.record_fill(ts_ns, notional)
    fee = s.fee_for(ts_ns, notional, is_maker=False)
    ```

=== "QuickJS"

    ```javascript
    const h = __flox_fee_schedule_create();
    __flox_fee_schedule_load_profile(h, "binance_um_futures");
    __flox_fee_schedule_record_fill(h, tsNs, notional);
    const fee = __flox_fee_schedule_fee_for(h, tsNs, notional, 0);
    ```

=== "C++"

    ```cpp
    auto s = flox::FeeSchedule::binance_um_futures();
    s.recordFill(tsNs, notional);
    double fee = s.feeFor(tsNs, notional, /*isMaker=*/false);
    ```

## Canned profiles

| Profile               | Tiers | Top maker rate |
|-----------------------|-------|----------------|
| `binance_um_futures`  | 10    | -0.005% rebate  |
| `bybit_linear`        | 6     | -0.005% rebate  |
| `okx_swap`            | 4     | 0%              |
| `deribit`             | 2     | -0.010% rebate above LV-1 |

Numbers reflect published VIP brackets at the time of writing.
Tune for your actual tier; venue schedules drift.

## Fee sign convention

Returned fee is positive when the account pays, negative when the
account receives (maker rebate). Integrate as:

```
equity -= sched.fee_for(ts_ns, notional, is_maker)
```

So a maker rebate (negative fee) *adds* to equity.

## Tier transition log

`tier_transition_ts_ns()` returns the timestamps of every tier
change recorded by `record_fill`. Useful for post-trade attribution
("which fills happened at each tier"):

```python
transitions = sched.tier_transition_ts_ns()
print(f"changed tier {len(transitions)} times during backtest")
```

## Notes

- The rolling window is exactly 30 days (`30 * 24 * 3600 * 1e9` ns).
  Notional ages out on the next call after the cutoff.
- Tiers are stored sorted ascending by `min_notional_30d`. Insert
  order does not matter; the schedule sorts on every `add_tier`.
- The active tier is resolved on every `fee_for` / `current_tier_index`
  call (cheap — linear walk of typically < 10 tiers).
- `reset_rolling()` clears the 30-day window and the transition log
  (keeps the tier definitions).
- For research that wants to disable fees entirely, instantiate an
  empty `FeeSchedule()` — `fee_for` returns 0 for every fill.
