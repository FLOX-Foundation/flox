# Position valuation

The position tracker values an open position linearly: unrealized PnL is
quantity times the difference between the current price and the average
entry. That is correct for spot and perpetual positions, where value moves
one for one with price.

## Where linear valuation breaks

Some positions do not move linearly with price. An AMM liquidity position
loses value to impermanent loss and gains it from accrued fees, and its
exposure changes across its price range. An option's value bends with
volatility and time. Pricing any of these as quantity times a price
difference gives the wrong number, which then flows into every risk check
that reads unrealized PnL.

## The hook

`AggregatedPositionTracker` takes an optional `IPositionValuator`. When none
is set, valuation stays linear and spot and perpetual positions are
unchanged. A position whose value is nonlinear sets its own valuator, which
receives the symbol, quantity, average entry, and current price, and returns
the unrealized PnL.

The symbol is part of the call so a stateful valuator can key its own
per-position data. An AMM valuator, for example, looks up the pool range,
the liquidity, and the accrued fees for that symbol and computes the value
from the pool's own math rather than from a single average entry price.

A set valuator is consulted whenever the tracker is asked for unrealized PnL,
including when the linear position is zero. An LP or option position is not a
tracked quantity, so its value comes entirely from the valuator's own state.
Only the linear default treats a zero position as zero PnL.

## What stays in the engine

The engine owns the hook and the linear default. It does not own the
nonlinear math. An LP or option valuator lives with the component that
tracks the extra state it needs, which for a DEX position is the connector
that manages the pool. Cross-venue aggregation is unchanged: positions still
net across venues, and only the final valuation step routes through the
valuator.
