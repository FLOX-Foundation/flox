# On-chain order lifecycle

A centralized-exchange order moves through a synchronous lifecycle:
submitted, accepted, then partially filled, filled, canceled, or rejected.
Each transition is a definite answer from the venue. An on-chain (DEX) order
does not work that way. It is probabilistic until the chain confirms it, and
it can fail after it looks accepted.

## What is different on-chain

A transaction sits in the mempool before any block includes it. While it
waits, it is neither accepted nor rejected, just pending. The chain can then
revert it (a swap whose price moved past its slippage bound, for example), so
a transaction that was broadcast successfully still ends with no fill. A
sender can also re-broadcast the same intent with higher gas to get it mined
sooner, which supersedes the earlier pending transaction.

A strategy that treats a broadcast transaction as filled will book positions
that never happened.

## The added states

FLOX extends OrderEventStatus with three connector-driven states:

- PENDING_ONCHAIN: broadcast to the mempool, not yet confirmed.
- REVERTED: the chain rejected the transaction. The reason rides on the
  event's reject-reason field.
- REPLACED_GAS: re-broadcast with higher gas, superseding the pending
  transaction.

OrderEvent also carries on-chain metadata that the connector fills: a
transaction hash and a confirmation count. The default values are empty and
zero, so centralized-exchange and backtest events are unchanged and existing
listeners that do not override the new callbacks keep working.

## Where the boundary sits

FLOX owns the state machine and the event shape. It does not talk to a chain.
RPC calls, mempool watching, gas policy, and signing live in the connector
that drives these transitions and fills the hash and confirmation count. The
engine's job is to model the states so a strategy can wait for confirmation
before acting, and react to a revert instead of assuming a fill.
