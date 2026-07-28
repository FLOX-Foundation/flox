"""flox_venue_guide — the venue module surface for AI agents.

FLOX is normally used to build strategies that trade *on* a market. The
optional venue module lets you build the market itself: order-level
matching, clearing, derivatives risk, market data, and a network
perimeter. An agent asked to "simulate several traders against one
book", "model market impact with reacting counterparties", or "run a
matching engine" has no way to discover that surface from the
strategy-side tools, so it lives here.

Bundled text (no live data); update alongside the venue docs.
"""
from __future__ import annotations


_GUIDE = """# FLOX venue module

Build a market rather than trade on one. Optional module, same shape as
`connectors/`: target `flox::venue`, include prefix `<flox-venue/...>`,
namespace `flox::venue`, gated by `-DFLOX_BUILD_VENUE` (build without it
entirely with `=OFF`).

Full docs: https://flox-foundation.github.io/flox/venue/

## When to reach for it

- **Multi-agent simulation.** Many strategies or agents trading against
  ONE book, so price, impact, and adaptation are emergent instead of
  assumed. A tape-replay backtest cannot express this: the recorded
  market never reacts to your orders.
- **Execution research.** Queue priority, iceberg refills, self-trade
  prevention, last-look rejection, auction uncrosses.
- **Running a venue.** Clearing, margin, liquidation, insurance, ADL,
  funding, market data, recovery, network perimeter.

If the user only wants a single strategy against historical data, the
ordinary backtest path (`run_backtest`) is the right tool, not this.

## Minimal venue

```cpp
#include "flox-venue/matching_engine.h"
#include "flox-venue/ledger.h"
#include "flox/book/matching_book.h"

using namespace flox;
using namespace flox::venue;

SymbolConfig cfg;
cfg.id = 1;
cfg.tickSize   = Price::fromDouble(0.01);
cfg.minPrice   = Price::fromDouble(1);      // 0 = unchecked
cfg.maxPrice   = Price::fromDouble(1000);
cfg.baseAsset  = 0;
cfg.quoteAsset = 1;

Ledger ledger;
ledger.deposit(acct, /*asset*/ 1, amountOf(Volume::fromDouble(10'000)));

MatchingEngine<MatchingBook> venue(cfg, [](const OutboundEvent& e) { publish(e); });
venue.setLedger(&ledger, /*venue account*/ 999);
venue.submit(InboundCommand{order}, tsNs);
```

Books: `MatchingBook` (map reference, obviously correct) and
`LadderBook` (tick-indexed, O(1) best, no steady-state allocation).
Interchangeable, and held identical by a differential fuzz.

## Surface map

| Need | Header |
|---|---|
| Matching, order lifecycle, auctions, LULD | `flox-venue/matching_engine.h`, `flox-venue/matcher.h` |
| Books | `flox/book/{matching_book,ladder_book}.h` (core) |
| Money (double entry, `__int128`, conservation-exact) | `flox-venue/ledger.h` |
| Portfolio margin, collateral haircuts, segregation | `flox-venue/{cross_margin,collateral,segregation}.h` |
| Mark/index price, feed circuit breaker | `flox-venue/{index_feed,mark_feed_driver}.h` |
| Funding (live rate + scheduler) | `flox-venue/{funding_rate,funding_scheduler}.h` |
| Single-writer runtime, WAL, replay determinism | `flox-venue/{sequenced_shard,journal,event_hash}.h` |
| Market data out (L2 + ITCH) | `flox-venue/{market_data,itch_codec}.h` |
| Gateways, sessions, FIX/OUCH/REST | `flox-venue/{tcp_gateway,ws_gateway,tls_gateway,session,fix_codec,ouch_codec,rest_json}.h` |
| Control plane, metrics | `flox-venue/{control_api,metrics,prometheus}.h` |

## Order vocabulary

Types LIMIT / MARKET / STOP / TAKE_PROFIT / TRAILING; TIF GTC, IOC,
FOK, GTD (`expiryNs`), POST_ONLY; iceberg (`visibleQuantity`); peg
(Bid/Ask/Mid + offset); OCO (`ocoGroup`); reduce-only; STP in four
modes with account or firm scope.

Every state-mutating input is an `InboundCommand` (`NewOrder`,
`CancelOrder`, `ModifyOrder`, `MassCancel`, `Quote`,
`LastLookDecision`, `SetMark`, `ApplyFunding`, `AdminCmd`) — that is
what makes deterministic journal replay possible.

## Derivatives

Set `cfg.linearPerp = true` plus `initialMarginBps` /
`maintenanceMarginBps`. Marks arrive as `SetMark` commands and drive
the maintenance sweep. Waterfall on a bankruptcy: cancel the account's
resting orders (freeing its own collateral) -> force-close at the mark
-> insurance fund -> ADL from the most profitable opposite side,
closed at the bankruptcy price.

For whole-account margin across symbols use `CrossMarginManager`.
Note the deliberate split: `flox::LiquidationEngine`
(`flox/backtest/`) is the backtest-side model on `double` equity;
`CrossMarginManager` is the venue-side one backed by the ledger in
`__int128`. Do not mix them for the same account.

## Multi-agent demo

`demo/src/multi_agent_venue_demo.cpp` — market maker, momentum,
mean-reversion and noise agents on one book.

```bash
cmake -S . -B build -DFLOX_BUILD_DEMO=ON
cmake --build build --target multi_agent_venue_demo
./build/demo/multi_agent_venue_demo
```

Emergent result: the maker earns the spread, momentum pays for
chasing, and cash plus inventory are conserved exactly. Two things to
get right when writing agents: quote on the tick grid (the venue
rejects anything else) and refresh quotes with `MassCancel`.

## Verification posture

The core is fuzz-proven in CI, not just unit-tested: a differential
fuzz (reference book vs ladder in lockstep, identical event stream, no
crossed book; `FLOX_FUZZ_OPS` for a deep run) and a conservation fuzz
(value never created or destroyed, and after the book drains every
account's `reserved` returns to zero). Plus an ASAN/UBSAN/TSAN gate:
`venue/scripts/run_sanitizers.sh`.

`venue/AUDIT-LOG.md` catalogues the defects this corpus caught, each
with the regression test that pins it.
"""


def flox_venue_guide() -> str:
    return _GUIDE
