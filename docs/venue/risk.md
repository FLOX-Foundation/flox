# Derivatives risk

A spot venue only has to move assets that exist. A derivatives venue extends
credit, so it needs margin, a defensible mark, a liquidation path, and a
backstop for when liquidation is not enough.

## Isolated margin (in the engine)

Set `SymbolConfig::linearPerp` and the engine clears a linear perpetual:

- **Initial margin on entry.** `notional * initialMarginBps` is reserved in quote
  collateral; insufficient margin rejects the order.
- **Netting positions** with an average entry price.
- **Mark-to-market PnL** realised into collateral through the venue clearing
  pool — a long's profit is funded by a short's loss, so value is conserved.
- **Reduce-only** orders are capped to the opposing position (they can never
  open or flip), re-checked on submit, on stop trigger, and on modify.
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

`SetMark` and `ApplyFunding` are **commands**, not method calls, so a replay
reproduces every mark-driven liquidation and funding payment exactly.

## Liquidation waterfall

1. **Cancel the account's resting orders.** Their initial margin is the
   account's own collateral; freeing it first means a total-equity-solvent
   account covers its own shortfall instead of the insurance fund paying out.
2. **Force-close at the mark** through the clearing pool.
3. **Insurance fund** (the venue account) absorbs a negative-equity deficit.
4. **ADL** claws the remainder from the most profitable opposite-side positions:
   each winner is closed at the bankruptcy price, forgoing exactly the gain that
   absorbs the deficit and keeping the excess. Victim selection uses a **total
   order** (`uPnl desc, then account, then symbol`) so it never depends on hash
   iteration or an unstable sort — a replica must pick the same victim.

## Portfolio (cross) margin

`CrossMarginManager` runs margin across an account's whole book instead of per
position, so a profit on one symbol offsets a loss on another.

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

`setLiquidationsPaused(true)` is the operator switch used when the price feed is
untrustworthy — see the circuit breaker below.

## Multi-asset collateral

`CollateralSchedule` values a basket with per-asset haircuts. An unconfigured
asset is worth **zero**, not full value, so forgetting to configure something is
safe rather than dangerous. During liquidation the venue buys the non-quote
collateral and the insurance fund only covers the remainder; every conversion is
balanced per asset.

## Mark and index price

A mark you can push is a mark someone will push. `index_feed.h` builds one that
resists that:

```cpp
IndexAggregator idx(/*stalenessNs*/ 5'000'000'000, /*maxDeviationBps*/ 500, /*minSources*/ 3);
idx.update(sourceId, price, tsNs);   // several external spot venues
idx.hasIndex(nowNs);                 // enough fresh sources?
idx.index(nowNs);                    // median, stale sources and outliers dropped
```

The mark is then the median of `{index, last trade, impact-mid}`, clamped into a
band around the index. **Impact-mid** is a VWAP walked into the *live book* to a
notional depth, not the top of book — so dust or a spoofed top level cannot move
the mark.

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

The live path is TWAP by default — sample the premium across the interval and
settle on the average, so a momentary dislocation cannot swing the payment:

```cpp
FundingCalculator f;
f.sample(mark, index);          // repeatedly across the interval
const double rate = f.intervalRate();
f.resetSamples();
```

`FundingScheduler` closes the loop (sample, settle at the boundary, reset).
Settlement is zero-sum: longs pay shorts or the reverse, and the charges across
a balanced book sum to zero.
