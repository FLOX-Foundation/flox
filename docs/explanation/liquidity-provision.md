# Liquidity-provision signals

A centralized-exchange strategy emits orders: buy, sell, cancel. An AMM
liquidity provider does something the order model cannot express. It deposits
two assets into a pool over a price range and later withdraws them. There is
no side and no fill price, just a range and an amount of liquidity.

## Why a new signal type

The Signal model is built around side, price, and quantity. None of those
describe "supply liquidity to this pool between 1900 and 2100." Forcing a
liquidity position into a limit order would lose the range, which is the part
that matters for an AMM position. So the model gains two signal types,
ProvideLiquidity and WithdrawLiquidity, that carry a price range and a
liquidity amount instead of a single price and side.

A strategy emits them the same way it emits any other signal:

- provideLiquidity(pool, priceLower, priceUpper, liquidity)
- withdrawLiquidity(pool, liquidity)

The pool is a registered symbol. The same helpers exist in the Python and
Node bindings.

## What executes them

Nothing on the centralized-exchange or backtest path. These signals flow
through the engine's event path like any other, but the backtest executor
and the CEX callback surface ignore them, because supplying pool liquidity is
an on-chain action. The connector that drives a DEX venue is what turns a
ProvideLiquidity signal into a pool deposit, signs it, and reports back
through the on-chain order lifecycle.

This keeps the boundary clean: the engine models the intent and carries it,
and the on-chain connector performs it. A strategy written against the engine
expresses what it wants without knowing which chain or pool implementation is
underneath.
