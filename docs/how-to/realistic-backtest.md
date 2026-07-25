# Realistic backtest in one call

Start every new backtest from a venue-typed factory. One call
wires the executor, the cross-margin account, the liquidation
engine, the fee schedule, the funding schedule, the rate-limit
policy, and the venue-availability hook to venue-realistic
defaults.

=== "Python"

    ```python
    import flox_py as flox

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
    import { VenueStack } from "@flox-foundation/flox";

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
| `deribit`                     | Bybit linear (Bybit ADL)   | 8h      | LV1 maker rebate path |

The `deribit` row carries two placeholders: there is no
deribit-specific `FundingSchedule` or `LiquidationEngine` profile
yet, so the factory wires `FundingSchedule::binance_um_futures()`
(8h) and `LiquidationEngine::bybit_linear()`. Fees, rate limits, and
the `pro_rata_with_fifo` queue model are deribit-specific. Override
the funding and liquidation components if the placeholders matter
for your research.

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
#      mark for the called symbol automatically).
#   2) run the liquidation walk.
acct.set_mark(BTC, 48_000.0)
outcome = liq.on_mark(BTC, 48_000.0)
if outcome["liquidations_count"] > 0:
    print("liquidated:", outcome["liquidated"])

# Multi-symbol: on_marks updates every attached account's mark for
# each (symbol, price) pair atomically, then walks liquidations once
# per symbol. Same aggregated outcome dict.
outcome = liq.on_marks([(BTC, 48_000.0), (ETH, 2_900.0)], ts_ns=now)

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

The factory wires defaults. Each accessor returns the live
component, so mutations after construction stick:

```python
# Switch ADL ranking strategy.
stack.liquidation().set_adl_ranking("position_size")
# Lower insurance fund to test cascade behaviour.
stack.liquidation().set_insurance_fund_capital(1_000.0)
# Override the placeholder funding rate.
stack.funding().set_constant_rate(0.0001)
```

`stack.executor()` returns a `VenueExecutor`, not a
`SimulatedExecutor`. Its full surface is `submit_order`,
`cancel_order`, `cancel_all`, `on_bar`, `on_trade`, `on_trade_qty`,
`fills_list`, `fill_count`, `set_rate_limit_policy`,
`clear_rate_limit_policy`, and `set_venue_availability`. The queue,
slippage, iceberg, latency, and STP knobs are fixed by the factory
and are not reachable through it — build a `SimulatedExecutor`
directly (or use `assemble_custom_venue` below) when you need them.

## Fully custom venue

For venues outside the canned set — or configurations where one
subsystem (fees, funding cadence, MM ladder) needs full
replacement rather than tuning — `assemble_custom_venue` wires
user-built subsystems into a venue-stack-shaped bundle:

=== "Python"

    ```python
    import flox_py as flox

    acct = flox.Account(account_id=42, equity=10_000)
    fees = flox.FeeSchedule()
    fees.add_tier(0, 1.0, 3.0)
    fees.add_tier(50_000, 0.5, 2.5)
    # 4h cadence; interval is fixed at construction, there is no setter.
    funding = flox.FundingSchedule.constant(4 * 3600 * 1_000_000_000, 0.0)
    liq = flox.LiquidationEngine()
    liq.add_tier(0.0, 0.004)
    rate_limits = flox.RateLimitPolicy()
    rate_limits.add_bucket("trading", 1_000_000_000, 50)

    custom = flox.assemble_custom_venue(
        account=acct, fees=fees, funding=funding, liquidation=liq,
        rate_limits=rate_limits, venue_name="my_exchange",
    )
    # custom.executor(), custom.account(), custom.liquidation(), ...
    ```

The helper:

- Creates a fresh executor and installs the rate limits + venue
  availability on it
- Binds fees to the account so 30d notional aggregates correctly
- Attaches the account to liquidation and routes liquidation
  orders through the executor

`CustomVenue` has the same accessor surface as a `VenueStack`
(`.executor()`, `.account()`, etc.) so downstream code is
interchangeable with a canned factory's output.

The C++ escape hatch is `VenueStack::assemble(AssembleArgs&&)`.
Codon and QuickJS users assemble manually via the existing setter
API.

## See also

- [Cross-margin accounts](cross-margin.md) — Account API details
- [Liquidation and ADL](liquidation-and-adl.md) — engine internals
- [Perpetual funding](perpetual-funding.md) — funding tape options
- [Rate limits](rate-limits.md) — per-endpoint budgets
- [Venue downtime](venue-downtime.md) — outage policies
