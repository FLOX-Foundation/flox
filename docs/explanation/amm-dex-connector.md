# AMM DEX connector

The rest of the engine consumes two market-data events: a book update and a
trade. A centralized exchange connector receives those from a venue and emits
them. An AMM pool has neither a book nor a trade feed in that shape. The
reference AMM connector bridges the gap by presenting a constant-product pool
through the same IExchangeConnector interface, so the engine drives a DEX
venue without knowing it is a pool.

## Pool state becomes a book

When the pool's reserves change, the connector synthesizes a snapshot book
around the spot price and emits it as a BookUpdateEvent. Each level is priced
off the curve: the further into the book, the larger the swap that level
represents, and the more the price impact moves it away from spot. Bids sit
below spot and asks above, both widening with depth. A strategy that reads a
book sees liquidity that thins out the way an AMM's does, without any special
casing.

A swap against the pool becomes a TradeEvent at the realized rate, and the
connector republishes the book because the reserves moved.

## How the pieces compose

The connector is where the DEX pieces meet. Reserves drive the synthetic book
through the AMM pricing curve. The venue registers as an AMM type, so the
routing flags select pool pricing and on-chain settlement. A position on the
pool carries a nonlinear valuator, and an order on it moves through the
on-chain lifecycle. Each piece works on its own, and the connector composes
them into a venue the engine treats like any other.

## Where the boundary holds

This is the engine-side skeleton. It maps pool state that is fed to it into
events; it does not reach a chain. Sourcing reserve updates from a node,
signing swaps, watching the mempool, setting gas, and protecting against MEV
live in the downstream connector that wraps this one. That split is the whole
point of the architecture: the engine models the venue, and the chain-specific
work stays outside it. The synthetic book here is a reference approximation,
pricing both sides from the same impact curve; a production connector would
price each side from its own swap direction.
