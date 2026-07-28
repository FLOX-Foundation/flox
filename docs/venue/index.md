# The venue module

Everything else in flox helps you **trade on a market**. This module lets you
**be the market**: match participants' orders against each other, clear the
money that changes hands, and manage the risk of standing in the middle.

It is an optional module — the same shape as `connectors/` — so the core library
stays lean for people who never need it. See
[Building with and without the venue](build.md).

```cpp
#include "flox-venue/matching_engine.h"
#include "flox-venue/ledger.h"
#include "flox/book/matching_book.h"

namespace venue = flox::venue;   // qualify venue types; see the namespace note below
using namespace flox;

venue::Ledger ledger;
ledger.deposit(/*account*/ 1, /*asset*/ 0, venue::amountOf(Quantity::fromDouble(10)));

venue::MatchingEngine<MatchingBook> engine(cfg, [](const venue::OutboundEvent&) { /* publish */ });
engine.setLedger(&ledger, /*venue account*/ 999);
engine.submit(venue::InboundCommand{order});
```

## Why this exists

A backtest replays a recorded tape. Whatever you do, the tape does not care:
your fills never move the price, your size never runs out of liquidity, and no
counterparty ever adapts. That is fine for a first pass and misleading for
anything sensitive to impact, queue position, or crowding.

With a venue in the loop the market **is** the other participants. Price becomes
an emergent result of who is quoting and who is taking, and every fill costs
someone the other side. That enables things a tape cannot express:

- **Multi-agent simulation** — many strategies (or AI agents) trading against
  one book, with impact and adaptation as emergent behaviour. See
  [Multi-agent simulation](multi-agent.md).
- **Realistic execution research** — queue priority, iceberg refills, self-trade
  prevention, last-look rejection, auction uncrosses.
- **Running an actual venue** — clearing, margin, liquidation, insurance, ADL,
  funding, market data, recovery, and a network perimeter.

## The pieces

| Area | What it does | Read more |
|---|---|---|
| Books | Order-level price-time and pro-rata matching books | [Matching](matching.md) |
| Matching engine | Order lifecycle: TIF, stops, OCO, peg, iceberg, auctions, LULD | [Matching](matching.md) |
| Ledger | Double-entry money, conservation-exact | [Clearing](clearing.md) |
| Derivatives risk | Margin, liquidation, insurance, ADL, funding, mark price | [Risk](risk.md) |
| Runtime | Single-writer sequenced core, journal, deterministic replay | [Runtime and recovery](runtime.md) |
| Market data | Venue events to an L2 feed (ITCH-style) | [Market data](market-data.md) |
| Perimeter | TCP/WS/TLS/UDP gateways, sessions, FIX/OUCH/REST codecs, control plane | [Perimeter](perimeter.md) |

## Design notes

- **Namespace `flox::venue`.** Nested, so venue vocabulary can reuse names the
  strategy side already owns — `Trade` and `SymbolConfig` exist in both. Scalars
  (`Price`, `Quantity`, `Side`, `OrderId`) come from `flox/common.h` as
  everywhere else.

    Because the two sets overlap, do **not** pull both in unqualified: a
    translation unit that does `using namespace flox;` *and*
    `using namespace flox::venue;` makes `Trade` and `SymbolConfig` ambiguous as
    soon as it also includes `flox/book/trade.h` or `flox/engine/engine_config.h`.
    Prefer `namespace venue = flox::venue;` and qualify (`venue::SymbolConfig`),
    or import only what you need.
- **Include prefix `<flox-venue/...>`**, target `flox::venue`.
- **Order-level books live in core** (`flox/book/`), next to the aggregate
  `NLevelOrderBook`, because they are useful on their own. Everything that only
  makes sense for a venue lives in the module.
- **Money is `__int128` fixed point**, never `double`. See
  [Clearing](clearing.md) for why the venue and the backtest keep separate
  margin models.

## Verification

The venue core is fuzz-proven in flox CI, not just unit-tested:

- **Differential fuzz** — the reference book and the O(1) ladder are driven in
  lockstep over a random command stream; they must emit a byte-identical event
  stream and never leave a crossed book.
- **Conservation fuzz** — value is never created or destroyed, and after the
  book is drained every account's `reserved` returns to zero.

Plus a sanitizer gate (ASAN/UBSAN/TSAN) over the parsers, fuzzes and concurrent
paths. See [Verification](verification.md), and `venue/AUDIT-LOG.md` for the
defects this corpus caught while the module was hardened.
