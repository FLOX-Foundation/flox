# Derivatives risk

A spot venue only moves assets that exist. A derivatives venue extends credit,
so it needs margin, a defensible mark, a liquidation path, and a backstop for
when liquidation is not enough.

## Isolated margin (in the engine)

Set `SymbolConfig::linearPerp` and the engine clears a linear perpetual:

- **Initial margin on entry.** `notional * initialMarginBps` is reserved in
  quote collateral; insufficient margin rejects the order. An order with no
  price of its own (market, triggered stop) is margined at the top of the price
  band on **both** sides -- a perp delivers nothing, so its notional grows with
  price whether the account ends long or short.
- **Netting positions** with an average entry price.
- **Mark-to-market PnL** realised into collateral through the venue clearing
  pool: a long's profit is funded by a short's loss, so value is conserved.
- **Reduce-only** orders are capped to the opposing position on submit, on stop
  trigger and on modify -- and re-measured **at fill time**, against the position
  as it is then. A resting order is sized against the position it saw when it
  was admitted; by the time it trades that position may be smaller or gone, so
  only the part that still reduces executes and the rest is canceled
  (`ReduceOnlyNotReducing`). That is what makes "never opens or flips" true of
  the fill and not just of the submit -- and it matters because a reduce-only
  order reserves no margin, so any part of it that opened a position would open
  one with none.
- **`maxPositionQty`** is enforced against the **resulting** position at fill
  time, not only against the incoming order: orders that each pass the cap
  individually cannot settle into a position past it together. The bite that
  would breach it is cut, and a maker with no room left is pulled
  (`PositionLimitExceeded`).
- **Maintenance sweep.** `setMarkPrice` walks every position; anything whose
  equity (posted margin + unrealised PnL, plus a negative wallet if the account
  is overdrawn) falls under the maintenance requirement is force-closed.

```cpp
cfg.linearPerp = true;
cfg.initialMarginBps = 1000;      // 10x
cfg.maintenanceMarginBps = 500;
cfg.autoDeleverage = true;

venue.submit(InboundCommand{SetMark{SYM, Price::fromDouble(49'500)}}, tsNs);
```

`SetMark` and `ApplyFunding` are commands in the sequenced stream, so a replay
reproduces every mark-driven liquidation and funding payment exactly.

## Liquidation waterfall

1. **Cancel the account's resting orders.** Their initial margin is the
   account's own collateral; freeing it first means a total-equity-solvent
   account covers its own shortfall before the insurance fund pays anything.
2. **Force-close at the mark** through the clearing pool.
3. **Insurance fund** (the venue account) absorbs a negative-equity deficit.
4. **ADL** claws the remainder from the most profitable opposite-side
   positions: each winner is closed at the bankruptcy price, giving up exactly
   the gain that absorbs the deficit and keeping the excess. Victim selection
   uses a total order (`uPnl desc, then account, then symbol`), so it does not
   depend on hash iteration or an unstable sort and a replica picks the same
   victim.

## Portfolio (cross) margin

`CrossMarginManager` runs margin across an account's whole book, so a profit
on one symbol offsets a loss on another.

```cpp
CrossMarginManager cm(ledger, /*collateral asset*/ USD, VENUE, onLiquidation, /*adl*/ true);
cm.configureSymbol(BTC, /*imBps*/ 1000, /*mmBps*/ 500);
cm.setMark(BTC, Price::fromDouble(50'000));

cm.canOpen(acct, BTC, Side::BUY, qtyRaw, priceRaw);  // pre-trade gate
cm.applyFill(acct, BTC, Side::BUY, qtyRaw, priceRaw);
cm.equity(acct);            // collateral basket + unrealised PnL
cm.withdrawable(acct);      // never lets collateral behind open positions leave
cm.openInterestRaw();       // venue-wide risk gauge
```

`setLiquidationsPaused(true)` is the operator switch for an untrustworthy
price feed; see the circuit breaker below.

## Handing risk to an owner outside the engine

Isolated margin is the engine's own business: it sees one instrument and the
collateral behind it, so it can decide alone. Portfolio margin cannot work that
way -- a hedge only nets across instruments, and the engines are per symbol. So
the decision moves out and the engine executes it, over three seams.

**Permission on entry.** `setCreditCheck` is asked before an order is funded,
whether or not this engine posts collateral of its own. The request names the
instrument (the owner serves many), the order type and whether it reduces
exposure; the answer carries a reject reason, so a client refused by portfolio
margin is told that, not a flat funds error.

**Fills.** Every outbound event carries its owner account, so the risk layer
rebuilds each account's basket by subscribing to the same stream everyone else
reads. Nothing extra is needed.

**Closing.** `ForceClosePosition` closes a position on the owner's decision,
settling through exactly the path the engine's own sweep uses, so the events
and the replay are identical either way. Pair it with
`SymbolConfig::externalLiquidation`, which stops the engine liquidating on its
own: two systems closing the same position from different numbers is worse than
either doing it alone.

What stays outside: how the requirement is computed, which positions net, what
haircuts collateral takes, when to close and in what order. Those differ per
venue and are the owner's to define.

## Multi-asset collateral

`CollateralSchedule` values a basket with per-asset haircuts. An unconfigured
asset is valued at zero, so a forgotten entry under-counts collateral instead
of extending credit against it. During liquidation the venue buys the
non-quote collateral and the insurance fund only covers the remainder; every
conversion is balanced per asset.

## Mark and index price

A mark you can push is a mark someone will push. `index_feed.h` builds one
that resists that:

```cpp
IndexAggregator idx(/*stalenessNs*/ 5'000'000'000, /*maxDeviationBps*/ 500, /*minSources*/ 3);
idx.update(sourceId, price, tsNs);   // several external spot venues
idx.hasIndex(nowNs);                 // enough fresh sources?
idx.index(nowNs);                    // median, stale sources and outliers dropped
```

The mark is the median of `{index, last trade, impact-mid}`, clamped into a
band around the index. Impact-mid is a VWAP walked into the live book to a
notional depth, so dust or a spoofed top level cannot move the mark.

## Feed circuit breaker

`MarkFeedDriver` watches the feed and pauses liquidations when it goes stale:

```cpp
MarkFeedDriver driver(...);
const bool publish = driver.onTick(nowNs, lastTradeRaw, impactMidRaw);
driver.paused();          // liquidations halted while the index is not fresh
driver.markAgeNs(nowNs);  // export as a health gauge
```

Liquidating traders on a frozen price is how venues turn an outage into an
incident. The breaker makes that failure mode explicit and observable.

## Funding

Two sources behind one settlement path:

- **`FundingSchedule`** (`flox/backtest/`) replays a recorded rate tape.
- **`FundingCalculator`** (`flox-venue/funding_rate.h`) computes the rate live:
  premium (mark vs index) plus a clamped interest component, capped.

The live path is TWAP by default: sample the premium across the interval and
settle on the average, so a momentary dislocation cannot swing the payment.

```cpp
FundingCalculator f;
f.sample(mark, index);          // repeatedly across the interval
const double rate = f.intervalRate();
f.resetSamples();
```

`FundingScheduler` closes the loop (sample, settle at the boundary, reset).
Settlement is zero-sum: longs pay shorts or the reverse, and the charges
across a balanced book sum to zero.
