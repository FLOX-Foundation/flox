# Clearing and the ledger

A matching engine that only moves quantities is a toy. `Ledger` is what makes it
a venue that moves money.

## Model

Double entry, multi-asset. Every account holds, per asset, an `available` and a
`reserved` balance in raw fixed point (1e-8 units) stored as `__int128` — wide
enough that a notional can never overflow, and never `double`, so a rounding
drift is impossible rather than merely unlikely.

```cpp
Ledger led;
led.deposit(acct, USD, amountOf(Volume::fromDouble(10'000)));

led.reserve(acct, USD, amt);        // available -> reserved (fails if short, no partial)
led.release(acct, USD, amt);        // reserved -> available
led.spendReserved(acct, USD, amt);  // reserved is paid out on a fill
led.credit(acct, BTC, amt);         // signed
led.debit(acct, USD, amt);          // all-or-nothing
```

Attach it to the engine and settlement happens automatically:

```cpp
venue.setLedger(&ledger, /*venue account*/ 999);
```

## Lifecycle of buying power

1. **Order entry reserves.** A bid reserves quote (`limitPrice * quantity`), an
   ask reserves base. If the reservation fails the order is rejected — this is
   the pre-trade buying-power gate.
2. **A fill settles.** Base and quote change hands, the over-reservation (price
   improvement) is released, fees move to the venue account.
3. **Every exit releases.** Cancel, IOC/FOK/MARKET residual, post-only rejection,
   LULD rejection, GTD expiry, STP, mass-cancel, liquidation, last-look reject —
   each returns the unspent reservation.

That last point is where venues leak. A reservation stranded in `reserved` is
invisible to a conservation-of-total check, because the money never left the
account — it is simply frozen forever. The fuzz therefore asserts a second,
stronger property: after the book is drained, **every account's `reserved` is
zero**. See [Verification](verification.md).

## Conservation

Assets are only ever exchanged, so each asset's total across all accounts plus
the venue account is invariant. The conservation fuzz checks this after every
command over a random stream of all order types.

Note the distinction the [multi-agent demo](multi-agent.md) makes explicit:
cash and inventory are conserved *exactly*, while total mark-to-market equity
moves with the last price. That is valuation, not leakage.

## Fee policy

Fees flow through `flox::FeeSchedule` (maker rebate / taker fee, volume tiers).
The venue gate reserves the **notional**, and the taker fee is charged
post-settlement. A taker with exactly the notional therefore ends fee-sized
negative rather than being pre-rejected: conservation still holds, the venue
still collects, and the deficit is bounded by the fee. This is a deliberate
policy choice, pinned by a test, not an oversight.

## Two margin models, on purpose

`flox::venue::CrossMarginManager` and `flox::LiquidationEngine`
(`flox/backtest/`) both liquidate. They are not duplicates — they answer
different questions:

| | `LiquidationEngine` (backtest) | `CrossMarginManager` (venue) |
|---|---|---|
| Money | `double` equity | `Ledger`-backed `__int128` |
| Driven by | a mark tape | live venue state |
| Optimised for | speed, cascade modelling, ADL ranking, mark impact | exactness: conservation per asset, multi-asset collateral haircuts, segregation |

Collapsing them would either put `double` money on the venue path or force
ledger machinery into backtests. It is the same split flox already makes between
`SimulatedClock` and `SystemClock`: one concept, two drivers. Pick by what you
are building; do not mix them for the same account.

## Segregation

`SegregationReport` answers the compliance question: is client money fully
backed by the segregated custody balance?

```cpp
SegregationReport seg(ledger, /*house accounts*/ {VENUE, INSURANCE});
seg.clientTotal(USD);              // sum of each client's POSITIVE balance
seg.fullyBacked(USD, custody);
seg.shortfall(USD, custody);       // > 0 is a reportable breach
```

Client money owed is the sum of **positive** obligations per account. A client's
debit balance is the firm's receivable, not a reduction of what it owes other
clients — netting it would understate the requirement and let a real custody
shortfall report as fully backed.
