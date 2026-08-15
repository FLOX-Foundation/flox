# Clearing and the ledger

`Ledger` holds the account balances and settles every fill the engine
produces.

## Model

Double entry, multi-asset. Every account holds, per asset, an `available` and a
`reserved` balance in raw fixed point (1e-8 units) stored as `__int128`. The
width means a notional cannot overflow, and integer money means there is no
rounding drift to accumulate.

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
   ask reserves base. If the reservation fails the order is rejected; this is
   the pre-trade buying-power gate.
2. **A fill settles.** Base and quote change hands, the over-reservation (price
   improvement) is released, fees move to the venue account.
3. **Every exit releases.** Cancel, IOC/FOK/MARKET residual, post-only
   rejection, LULD rejection, GTD expiry, STP, mass-cancel, liquidation,
   last-look reject: each returns the unspent reservation.

That last point is where venues leak. A reservation stranded in `reserved` is
invisible to a conservation-of-total check, because the money stayed in the
account; it is simply frozen. The fuzz therefore asserts a second property:
after the book is drained, every account's `reserved` is zero. See
[Verification](verification.md).

Two rules keep step 3 from turning into step 2's problem:

- **A removal resolves last-look holds first.** Every path that pulls an order
  -- including the ones the matcher owns itself, self-trade prevention and a
  fill-time risk block -- resolves that order's open holds before releasing its
  reservation. Releasing first would hand the collateral back while a hold that
  still has to settle from it is open.
- **A leg that cannot pay does not settle.** `Ledger::debit` is all-or-nothing.
  Both unreserved legs of a fill are funded before anything is credited, and if
  either refuses the settlement is abandoned as a unit: nothing moves, the trade
  is counted in `unsettledTrades()`, and the venue does not invent the
  difference. That counter must read zero.

## Conservation

Assets are only ever exchanged, so each asset's total across all accounts plus
the venue account is invariant. The conservation fuzz checks this after every
command, over a random stream covering limits, market and stop orders, GTD,
OCO, pegs, icebergs, modify and cancel, last look under every STP mode, and
perp fills with liquidation and ADL.

What the fuzz does NOT drive, so read its green run accordingly: mass cancel,
market-maker protection, halt-and-cancel-all, Deposit/Withdraw commands,
two-sided quote replace, and engine-level funding application. Those paths have
point tests, not randomized ones.

Cash and inventory are conserved exactly. Total mark-to-market equity moves
with the last price, which is valuation, not a leak; the
[multi-agent demo](multi-agent.md) prints both so the difference is visible.

## Per-symbol scale

The default fixed-point scale (1e8) caps an int64 quantity at about 9.2e10
whole units. That is too small for tokens whose circulating supply runs to
1e14. Such a symbol sets `priceScale`/`qtyScale` on its `SymbolConfig` (and on
`configureSymbol` for cross margin), and its price and quantity raws are then
interpreted in those units end to end: matching, reservation, settlement,
margin, PnL, funding, fees, and metrics.

Two invariants keep the money model simple:

- **Money never changes scale.** Every quote amount in the ledger is at the
  fixed 1e-8 scale (`venue::kMoneyScale`) regardless of the symbol that
  produced it, so balances from symbols with different scales add up safely.
- **A base-asset balance is the symbol's qty raw.** All symbols sharing a base
  asset must therefore use one `qtyScale`.

Scales must be positive, and `qtyScale` must divide or be a multiple of the
money scale; any power of ten works. `venue::scalesValid` checks the pair. At
the default scale every formula is bit-identical to the fixed-scale code it
replaced, so determinism hashes and replays are unaffected.

## Fees

Fees flow through `flox::FeeSchedule` (maker rebate / taker fee, volume tiers).
The venue gate reserves the notional, and the taker fee is charged
post-settlement. A taker holding exactly the notional therefore ends fee-sized
negative instead of being pre-rejected. Conservation still holds, the venue
still collects, and the deficit is bounded by the fee; a test pins this
behaviour as the intended policy.

## Two margin models

`flox::venue::CrossMarginManager` and `flox::LiquidationEngine`
(`flox/backtest/`) both liquidate. They answer different questions:

| | `LiquidationEngine` (backtest) | `CrossMarginManager` (venue) |
|---|---|---|
| Money | `double` equity | `Ledger`-backed `__int128` |
| Driven by | a mark tape | live venue state |
| Optimised for | speed, cascade modelling, ADL ranking, mark impact | exactness: conservation per asset, multi-asset collateral haircuts, segregation |

Collapsing them would either put `double` money on the venue path or force
ledger machinery into backtests. It is the same split FLOX makes between
`SimulatedClock` and `SystemClock`: one concept, two drivers. Pick by what you
are building, and keep one account on one model.

## Segregation

`SegregationReport` answers the compliance question: is client money fully
backed by the segregated custody balance?

```cpp
SegregationReport seg(ledger, /*house accounts*/ {VENUE, INSURANCE});
seg.clientTotal(USD);              // sum of each client's POSITIVE balance
seg.fullyBacked(USD, custody);
seg.shortfall(USD, custody);       // > 0 is a reportable breach
```

Client money owed is the sum of positive obligations per account. A client's
debit balance is the firm's receivable; netting it against other clients'
balances would understate the requirement and let a real custody shortfall
report as fully backed.
