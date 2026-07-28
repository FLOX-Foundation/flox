# Matching

## Two books, one interface

Order-level books live in core (`flox/book/`) and are distinct from
`NLevelOrderBook`, which aggregates per-level totals for market data. Matching
needs individual orders: to keep time priority, cancel by id, and refill icebergs.

| Book | Shape | Use |
|---|---|---|
| `MatchingBook` | `std::map` + `std::list`, allocates | Reference oracle. Obviously correct, easy to reason about. |
| `LadderBook` | tick-indexed dense ladders, intrusive FIFO over a node pool, occupancy bitmap | Performance path. O(1) best, O(1) next level, no steady-state allocation. |

They are interchangeable (`MatchingEngine<MatchingBook>` /
`MatchingEngine<LadderBook>`) and are held to be **observationally identical** by
a differential fuzz — see [Verification](verification.md).

```cpp
LadderBook book(LadderBook::Config{
    .basePriceRaw = 0,
    .tickRaw      = Price::fromDouble(0.01).raw(),
    .numLevels    = 20000,     // price band height, in ticks
    .maxOrders    = 1 << 20}); // node pool capacity
```

## Matching policies

`Matcher<Book>` implements the allocation rule:

- **Price-time (FIFO)** — the default. Best price first; within a price, oldest
  order first.
- **Pro-rata** — allocation at a level is proportional to resting size, with the
  usual leftover pass.

```cpp
Matcher<MatchingBook> m(MatchPolicy::ProRata);
m.setStpGroup(/*account*/ 10, /*firm*/ 1);   // firm-scope self-trade prevention
```

### Self-trade prevention

Four modes, applied when the aggressor and the maker share an STP scope (the
account, or the firm group registered via `setStpGroup`):

| Mode | Effect |
|---|---|
| `CancelNewest` | the incoming order is killed |
| `CancelOldest` | the resting order is pulled, matching continues |
| `CancelBoth` | both go |
| `Decrement` | the smaller leg is removed, the larger is reduced by that size; no trade |

STP interacts with `FOK`: an all-or-none order cannot count liquidity it would
never be allowed to trade with, so the precheck excludes same-scope resting size.

## Order types and time in force

`NewOrder` carries the full venue vocabulary:

- **Types** — `LIMIT`, `MARKET`, `STOP_MARKET`, `STOP_LIMIT`, `TAKE_PROFIT*`,
  `TRAILING_STOP`.
- **TIF** — `GTC`, `IOC`, `FOK`, `GTD` (with `expiryNs`), `POST_ONLY`.
- **Iceberg** — `visibleQuantity` shows a peak and hides the rest; the hidden
  reserve is real liquidity for matching but never leaks to the public feed.
- **Peg** — `PegRef::{Bid,Ask,Mid}` plus a signed offset; repriced at each submit
  boundary, tick-aligned, clamped so it never crosses.
- **OCO** — `ocoGroup`; a fill on one leg cancels its siblings.
- **Reduce-only** — perp orders that may only reduce a position, re-capped on
  submit, trigger, and modify.

## The engine

`MatchingEngine<Book>` owns the lifecycle around the matcher: validation, risk
gates, stops, expiry, pegs, auctions, clearing, and event emission.

```cpp
SymbolConfig cfg;
cfg.id = 1;
cfg.tickSize = Price::fromDouble(0.01);
cfg.minPrice = Price::fromDouble(1);      // 0 = unchecked
cfg.maxPrice = Price::fromDouble(1000);
cfg.baseAsset = 0;
cfg.quoteAsset = 1;

MatchingEngine<MatchingBook> venue(cfg, [](const OutboundEvent& e) { publish(e); });
venue.submit(InboundCommand{order}, tsNs);
```

Every state-mutating input is an `InboundCommand`: `NewOrder`, `CancelOrder`,
`ModifyOrder`, `MassCancel`, `Quote`, `LastLookDecision`, `SetMark`,
`ApplyFunding`, `AdminCmd`. That is what makes deterministic replay possible —
see [Runtime and recovery](runtime.md).

### Pre-trade risk

Configured on `SymbolConfig` and adjustable live (operators tighten during
volatility without a restart):

| Control | Config | Live setter |
|---|---|---|
| Tick / lot / min quantity | `tickSize`, `lotSize`, `minQty` | — |
| Price band (collar) | `minPrice`, `maxPrice` | — |
| Fat finger | `maxOrderQty`, `maxOrderNotional` | `setFatFinger` |
| LULD volatility band | `luldBps`, `luldHaltNs` | `setLuldBps` |
| Max resting orders per account | `maxOpenOrders` | `setMaxOpenOrders` |
| Max position (perp) | `maxPositionQty` | `setPositionLimit` |
| Margin requirement | `initialMarginBps`, `maintenanceMarginBps` | `setMarginBps` |
| Halt | `halted` | `setHalted` |

### Market-maker features

- **Last look** — `lastLookWindowNs`: the maker holds a fill and answers with
  `LastLookDecision`. On reject or timeout the held quantity does not trade, and
  both sides' reservations are returned.
- **MMP** — `setMmp(account, qtyLimit, windowNs)`: if an account is filled faster
  than its limit inside the window, its book is pulled and `MmpTriggered` fires.
- **Mass quote** — `Quote` replaces both sides of a two-sided quote atomically.

### Auctions and halts

Sequenced through `AdminCmd`, so they survive replay:

- `BeginPreOpen` — accumulate without matching (a crossed book is allowed).
- `OpenContinuous` — uncross at the single volume-maximising price, then resume.
- `ResumeAuction` — clear a halt into a re-opening auction.
- `HaltAndCancelAll` — emergency: halt and pull the resting book.
- `Halt` / `Resume`.
