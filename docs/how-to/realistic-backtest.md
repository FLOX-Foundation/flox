# Realistic backtest in one call

Start every new backtest from a venue-typed factory. One call
wires the executor, the cross-margin account, the liquidation
engine, the fee schedule, the funding schedule, the rate-limit
policy, and the venue-availability hook to venue-realistic
defaults.

=== "Python"

    ```python
    import flox

    stack = flox.VenueStack.binance_um_futures(
        account_id=42, equity=10_000.0)

    acct = stack.account()
    liq = stack.liquidation()
    fees = stack.fees()
    funding = stack.funding()
    exec_ = stack.executor()
    ```

=== "TypeScript"

    ```typescript
    import { VenueStack } from "flox";

    const stack = VenueStack.binanceUmFutures(42, 10_000);
    stack.accountOpenPosition(1, 5.0, 50_000);
    const n = stack.liquidationOnMark(1, 47_000);
    ```

=== "Codon"

    ```python
    from flox.backtest import VenueStack

    stack = VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    acct = stack.account_handle()  # raw handle; wrap as Account if needed.
    ```

## Available venues

| Factory                       | Liquidation profile        | Funding | Fees                  |
| ----------------------------- | -------------------------- | ------- | --------------------- |
| `binance_um_futures`          | Binance UM (Binance ADL)   | 8h      | 10-tier VIP ladder    |
| `bybit_linear`                | Bybit linear (Bybit ADL)   | 8h      | 6-tier VIP ladder     |
| `okx_swap`                    | OKX swap (PnlRatio ADL)    | 8h      | 4-tier VIP ladder     |
| `deribit`                     | Deribit options (PnlRatio) | 1h      | LV1 maker rebate path |

String dispatch via `VenueStack.from_venue("binance", account_id,
equity)` is also available for codegen / agents that pick the
venue programmatically.

## Drive the stack

The factory returns a fully-wired stack. From there:

```python
# Open one or more positions on the cross-margin account.
acct.open_position(symbol=BTC, quantity=5.0, entry_price=50_000.0)

# On every tick of the strategy loop:
#   1) update marks (one per symbol you hold; the engine sets the
#      mark for the called symbol automatically — see T053 for the
#      multi-symbol atomic path when that lands).
#   2) run the liquidation walk.
acct.set_mark(BTC, 48_000.0)
outcome = liq.on_mark(BTC, 48_000.0)
if outcome["liquidations_count"] > 0:
    print("liquidated:", outcome["liquidated"])

# Record fills through the fee schedule so the 30-day VIP tier
# tracks correctly (FeeSchedule reads aggregate notional from the
# account when bound — which the factory does for you).
fees.record_fill(ts_ns=now, notional=100_000.0)
fee_paid = fees.fee_for(ts_ns=now, notional=100_000.0, is_maker=False)
```

## Why this is the default

There is no useful "unrealistic" backtest — omitting fees, funding,
liquidation, queue model, ack latency, or rate limits silently
produces optimistic numbers that do not survive contact with the
live venue. The bare `SimulatedExecutor()` constructor stays
available for unit tests of the executor itself, but research
backtests always go through a venue factory.

## Custom venues / overrides

The factory wires defaults. Tune individual components after
construction:

```python
# Bump iceberg refresh latency to 200ms (slower venue).
stack.executor().set_iceberg_refresh_latency(200_000_000)
# Switch ADL ranking strategy.
stack.liquidation().set_adl_ranking("position_size")
# Lower insurance fund to test cascade behaviour.
stack.liquidation().set_insurance_fund_capital(1_000.0)
```

For a fully custom venue not covered by the canned factories, use
`VenueStack.assemble(...)` (C++) or build the subsystems by hand
and connect them with the same pattern the factory uses.

## See also

- [Cross-margin accounts](cross-margin.md) — Account API details
- [Liquidation and ADL](liquidation-and-adl.md) — engine internals
- [Perpetual funding](perpetual-funding.md) — funding tape options
- [Rate limits](rate-limits.md) — per-endpoint budgets
- [Venue downtime](venue-downtime.md) — outage policies
