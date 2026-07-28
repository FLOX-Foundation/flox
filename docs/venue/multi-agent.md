# Multi-agent simulation

A tape-replay backtest has one participant: you. The recorded market cannot
respond to your orders, so impact, queue position, and crowding are assumptions
baked into a fill model rather than outcomes you observe.

Point several agents at one venue and those become emergent. Price is whatever
the interaction produces; every fill has a counterparty who now holds the other
side and reacts to it.

## Run the demo

```bash
cmake -S . -B build -DFLOX_BUILD_DEMO=ON
cmake --build build --target multi_agent_venue_demo
./build/demo/multi_agent_venue_demo
```

Source: `demo/src/multi_agent_venue_demo.cpp`.

Four agents share one book through the matching engine, settling through the
ledger:

| Agent | Behaviour |
|---|---|
| market maker | two-sided quotes around fair value, skewed as inventory builds |
| momentum | chases the previous round's move |
| mean reversion | fades deviation from an anchor, sized by how stretched it is |
| noise | small random market orders (uninformed flow) |

```
final price 99.93 after 32605 trades (0 rejects)

agent                    cash      stock            pnl
market-maker       1002001.71    1005.42       +2473.42
momentum            992094.14    1052.51       -2728.80
mean-reversion      996010.11    1047.09        +645.58
noise              1009894.05     894.98        -670.20

conservation   cash 4000000.00 -> 4000000.00 (drift +0.00000000)
               stock  4000.00 ->    4000.00 (drift +0.00000000)
```

The maker earns the spread and momentum pays for chasing. Nobody scripted that
distribution; it falls out of the interaction. Cash and inventory are conserved
exactly, while total mark-to-market equity moves with the last price, because
equity is a valuation.

## What this surfaces that a tape cannot

The demo is deliberately easy to destabilise, because the instability is the
point:

- **Momentum ignition.** Weaken the mean-reversion agent or deepen the momentum
  clip and price runs away until it pins against the price band. A tape replay
  cannot show this, because the recorded market does not chase your own flow.
- **Inventory limits.** The maker's skew is what stops it accumulating without
  bound. Remove it and the maker ends up holding the whole move.
- **Liquidity exhaustion.** Size the takers past the maker's quoted depth and
  fills start walking the book. The slippage is real, produced by the book
  itself instead of a fill-model parameter.

The demo prints a reject counter alongside trades for exactly this reason: when
the market stops working, you want to see *why*. During development it was a
non-zero reject count that revealed quotes drifting off the tick grid, which the
engine was correctly rejecting.

## Building your own

The pattern is small: construct a venue, deposit balances, then loop over your
agents submitting `InboundCommand`s and reading back the events.

```cpp
Ledger ledger;
MatchingEngine<MatchingBook> venue(cfg, [&](const OutboundEvent& e) {
  if (const auto* t = std::get_if<Trade>(&e)) { lastPrice = t->price.toDouble(); }
});
venue.setLedger(&ledger, VENUE_ACCOUNT);

for (int round = 0; round < rounds; ++round) {
  for (auto& agent : agents) agent.act(venue, lastPrice);
}
```

Two things to get right:

1. **Quote on the tick grid.** The venue rejects anything else
   (`TickSizeViolation`), as a real exchange does.
2. **Refresh quotes deliberately.** A maker that never cancels leaves stale
   orders behind; `MassCancel` per account is the usual way to pull a quote set.

For a multi-symbol simulation, put each symbol behind `SymbolRouter`, which
routes commands to the owning engine. There is no cross-symbol matching, so
symbols are independent — to actually run them concurrently, give each its own
`SequencedShard` (the router itself dispatches inline).
