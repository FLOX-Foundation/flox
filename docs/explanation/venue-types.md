# Venue types

An exchange registers with a venue type: a centralized exchange, an AMM DEX,
or a hybrid. The type had been a label that nothing read. It now drives the
two decisions that separate a DEX venue from a CEX one: how a swap is priced,
and how an order settles.

## What the type decides

`venueBehavior` maps a venue type to two flags. `usesOrderBook` chooses
between central-limit-order-book matching and AMM reserve pricing. A
centralized exchange and a hybrid both use a book; a pure AMM does not, and
its fills come from the pool curve instead. `onChainSettlement` chooses
between the synchronous CEX order lifecycle and the probabilistic on-chain one
where a transaction can sit pending, revert, or be re-broadcast with higher
gas. Both DEX types settle on-chain.

| Venue type | Uses order book | On-chain settlement |
|---|---|---|
| Centralized exchange | yes | no |
| AMM DEX | no | yes |
| Hybrid DEX | yes | yes |

## Reading it through a symbol

`SymbolRegistry::venueTypeForSymbol` resolves a symbol to its exchange's venue
type, defaulting to a centralized exchange when the symbol or its exchange is
unknown. A component that holds a symbol but not its exchange, an executor
deciding how to fill or a connector deciding how to settle, asks the registry
and then reads the behavior flags.

## Why a flag table

Routing code reads `usesOrderBook` and `onChainSettlement` rather than
switching on the enum in each place. A new venue type is described once, in
the behavior table, and every routing decision picks it up. The pricing
model, the lifecycle, and the valuation a venue needs all hang off these two
flags rather than off scattered checks for a specific enum value.
