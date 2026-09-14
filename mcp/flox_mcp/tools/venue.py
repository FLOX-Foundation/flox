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
#include "flox-venue/matching_book.h"

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
| Books | `flox/book/ladder_book.h` (core), `flox-venue/matching_book.h` (oracle) |
| Money (double entry, `__int128`, conservation-exact) | `flox-venue/ledger.h` |
| Portfolio margin, collateral haircuts, segregation | `flox-venue/{cross_margin,collateral,segregation}.h` |
| Mark/index price, feed circuit breaker | `flox-venue/{index_feed,mark_feed_driver}.h` |
| Funding (live rate + scheduler) | `flox-venue/{funding_rate,funding_scheduler}.h` |
| Single-writer runtime, WAL, replay determinism | `flox-venue/{sequenced_shard,journal,event_hash}.h` |
| Market data out (L2 + SBE) | `flox-venue/{market_data,sbe_md_codec}.h` |
| Gateways, sessions, FIX/SBE-OE/REST | `flox-venue/{tcp_gateway,ws_gateway,tls_gateway,session,fix_codec,sbe_order_entry_codec,rest_json}.h` |
| Control plane, metrics | `flox-venue/{control_api,metrics,prometheus}.h` |

## Order vocabulary

Types LIMIT / MARKET / STOP / TAKE_PROFIT / TRAILING; TIF GTC, IOC,
FOK, GTD (`expiryNs`), POST_ONLY; iceberg (`visibleQuantity`); peg
(Bid/Ask/Mid + offset); OCO (`ocoGroup`); reduce-only; STP in four
modes with account or firm scope. `Quote` carries the same controls,
so a two-sided quote gives up nothing a single order has.

Every state-mutating input is an `InboundCommand` (`NewOrder`,
`CancelOrder`, `ModifyOrder`, `MassCancel`, `Quote`,
`LastLookDecision`, `SetMark`, `ApplyFunding`, `AdminCmd`) — that is
what makes deterministic journal replay possible.

Journal and snapshot records carry a format version, and a build reads
only the version it writes. A file from another version is refused as a
`JournalFormatError` naming both numbers; a segment in that state stops
the shard from starting, a snapshot falls back a generation. Note that
`FLOX_SCALE_CHECKS` (on by default without `NDEBUG`) widens `Decimal`
and is therefore its own format version, so a journal written by a debug
build is refused by a release one rather than misread.

A `FOK` decides everything before it prints anything: it plans every
bite of the sweep against the risk limits, refuses if the plan is short,
and then does not re-ask. Nothing an agent does between the plan and the
sweep can leave it half filled.

`CancelOrder`, `ModifyOrder` and `Quote` address orders by id, and the
engine checks that the command's `accountId` owns the order it names
(`NotOrderOwner` otherwise). Order ids are one global namespace, so
the id alone is not authorization. `accountId == 0` is the unbound /
trusted-transport sentinel and keeps full control; an in-process
embedder acts as `0`.

A modify at the same price, shrinking, reduces in place and keeps
time priority. Any other amend re-enters at the tail carrying the
order's STP, reduce-only, post-only, last-look flag and iceberg peak,
and it can end as a rest, a fill, an `OrderRejected` or an
`OrderCanceled`. `newQty` on an iceberg means the total remaining,
peak plus hidden.

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

Two things it will not guess. `configureSymbol` comes before any
fill: `canOpen` refuses to grow exposure on a symbol with no margin
profile, and a leg that exists without one is charged full notional
(`setUnconfiguredSymbolMargin` moves that rate). A symbol with no
`setMark` is valued at each leg's entry price, so an unmarked
position is worth neither a gain nor a loss -- no zero marks reach
PnL, margin, ADL or the tape.

## Control plane requests

`ControlApi::handle` takes JSON and never fills a field in for you.
`setBand` needs `minPrice` and `maxPrice` together, paired risk
limits (`luldBps`+`luldHaltNs`, `maxOrderQty`+`maxOrderNotional`,
`initialMarginBps`+`maintenanceMarginBps`) need both halves, and
flags (`halted`, `open`, `delisted`) must be written as `true` or
`false`. Anything unparseable, non-finite or past the fixed-point
range answers `{"ok":false,"error":"bad_field"}`; `minPrice` above
`maxPrice` answers `bad_band`. `TcpControlServer` listens on
loopback only, caps a request line at 1 MiB, and has no
authentication of its own.

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

Order-integrity properties that no other suite asserts -- who may act
on an order, what a modify preserves, and what all-or-none is worth
when another gate cuts liquidity out from under the sweep -- live in
`venue/tests/test_venue_matching_integrity.cpp`.
"""


def flox_venue_guide() -> str:
    return _GUIDE
