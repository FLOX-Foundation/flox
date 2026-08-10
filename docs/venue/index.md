# The venue module

The rest of FLOX trades on a market. This module is the market: it matches
participants' orders against each other, clears the money that changes hands,
and manages the risk of standing in the middle.

It is an optional module. With `FLOX_BUILD_VENUE=OFF` nothing venue-related is
configured, compiled, or tested. See
[Building with and without the venue](build.md).

```cpp
#include "flox-venue/matching_engine.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"

namespace venue = flox::venue;   // qualify venue types; see the namespace note below
using namespace flox;

venue::Ledger ledger;
ledger.deposit(/*account*/ 1, /*asset*/ 0, venue::amountOf(Quantity::fromDouble(10)));

venue::MatchingEngine<MatchingBook> engine(cfg, [](const venue::OutboundEvent&) { /* publish */ });
engine.setLedger(&ledger, /*venue account*/ 999);
engine.submit(venue::InboundCommand{order});
```

## Why this exists

A backtest replays a recorded tape. Your fills never move the price, your size
never runs out of liquidity, and no counterparty adapts to you. That works for
a first pass and misleads on anything sensitive to impact, queue position, or
crowding.

With a venue in the loop the market is the other participants. Price emerges
from who is quoting and who is taking, and every fill puts the other side in
someone's hands. This is useful for:

- **Multi-agent simulation.** Many strategies (or AI agents) trade against one
  book, and impact and adaptation emerge from the interaction. See
  [Multi-agent simulation](multi-agent.md).
- **Execution research.** Queue priority, iceberg refills, self-trade
  prevention, last-look rejection, auction uncrosses.
- **Running an actual venue.** Clearing, margin, liquidation, insurance, ADL,
  funding, market data, recovery, and a network perimeter.

## The pieces

| Area | What it does | Read more |
|---|---|---|
| Books | Order-level price-time and pro-rata matching books | [Matching](matching.md) |
| Matching engine | Order lifecycle: TIF, stops, OCO, peg, iceberg, auctions, LULD | [Matching](matching.md) |
| Ledger | Double-entry money, conservation-exact | [Clearing](clearing.md) |
| Derivatives risk | Margin, liquidation, insurance, ADL, funding, mark price | [Risk](risk.md) |
| Runtime | Single-writer sequenced core, journal, deterministic replay | [Runtime and recovery](runtime.md) |
| Market data | Venue events to an L2 feed (SBE) | [Market data](market-data.md) |
| Perimeter | TCP/WS/TLS/UDP gateways, sessions, FIX/SBE-OE/REST codecs, control plane | [Perimeter](perimeter.md) |

## Design notes

**Namespace `flox::venue`.** Nested, so venue vocabulary can reuse names the
strategy side already owns; `Trade` and `SymbolConfig` exist in both. Scalars
(`Price`, `Quantity`, `Side`, `OrderId`) come from `flox/common.h` as
everywhere else.

Because the two sets overlap, avoid pulling both in unqualified. A translation
unit with `using namespace flox;` and `using namespace flox::venue;` makes
`Trade` and `SymbolConfig` ambiguous as soon as it includes
`flox/book/trade.h` or `flox/engine/engine_config.h`. Write
`namespace venue = flox::venue;` and qualify, or import only what you need.

**Include prefix `<flox-venue/...>`**, target `flox::venue`.

**Order-level books live in core** (`flox/book/`), next to the aggregate
`NLevelOrderBook`, because they are useful on their own. Everything that only
makes sense for a venue lives in the module.

**Money is `__int128` fixed point.** See [Clearing](clearing.md) for the money
model and for why the venue and the backtest keep separate margin models.

**Per-symbol scale.** A symbol whose price or supply range does not fit the
default 1e8 scale in int64 (a meme coin with a 1e14 supply) sets its own
`priceScale`/`qtyScale` in `SymbolConfig`. Money still settles at the fixed
1e-8 scale. See [Clearing](clearing.md#per-symbol-scale).

## Verification

The venue core is fuzzed in FLOX CI:

- **Differential fuzz.** The reference book and the O(1) ladder are driven in
  lockstep over a random command stream. They must emit a byte-identical event
  stream, and neither may end up crossed.
- **Conservation fuzz.** Each asset's total is invariant across every command,
  and after the book is drained every account's `reserved` returns to zero.

A sanitizer gate (ASAN/UBSAN/TSAN) covers the parsers, the fuzzes, and the
concurrent paths. See [Verification](verification.md).
