# Aggregate risk across strategies

Per-strategy daily-loss limits are common in trading frameworks. Portfolio-level caps that span strategies and accounts are commercial-only or homegrown. `flox_py.portfolio_risk` is the open-source reference implementation: a single-process, in-memory aggregator that combines PnL and exposure across N registered strategies, applies portfolio-level rules, and trips a kill switch when any rule is breached.

Multi-process aggregation through shared state is a Phase 2 concern. The API stays the same when that backend lands; nothing about how you call this changes.

## Quick start

```python
from flox_py.portfolio_risk import (
    PortfolioRiskAggregator,
    RiskRules,
)

aggregator = PortfolioRiskAggregator(
    rules=RiskRules(
        max_drawdown_pct=0.20,
        max_daily_loss=10_000,
        max_gross_exposure=500_000,
        max_concentration_pct=0.40,
    ),
    initial_equity=100_000,
    on_breach=my_kill_switch.activate,
)

# As each strategy reports its daily PnL and exposure:
aggregator.update(
    "ema-trend",
    realized_pnl=120.0,
    unrealized_pnl=-30.0,
    fees=-2.50,
    gross_exposure=5_000.0,
    net_exposure=4_500.0,
    trade_count=12,
)

# Pre-trade gating from inside a RiskManager hook:
breach = aggregator.check_order(
    strategy="ema-trend",
    notional=2_500.0,
    side="buy",
)
if breach is not None:
    reject_order(breach.detail)
```

## The four rules

| Rule | Trips when |
|---|---|
| `max_drawdown_pct` | `(peak_equity - current_equity) / peak_equity` exceeds the threshold. |
| `max_daily_loss` | Combined daily PnL across every strategy goes more negative than this. |
| `max_gross_exposure` | Sum of `gross_exposure` across all strategies exceeds this cap. |
| `max_concentration_pct` | Any single strategy holds more than this share of total gross exposure. Only fires with 2+ contributing strategies. |

Each rule is independent. Setting any to `None` disables that rule. A breach on any rule trips the kill switch and fires the optional `on_breach` callback once.

## Pre-trade gating

`check_order(strategy, notional, side)` inspects a candidate order against the current aggregate without mutating state. It returns either `None` (allowed) or a `Breach` describing why the order should be rejected. Use it from your `RiskManager` hook; it is cheap to call and thread-safe.

The gate applies the gross-exposure cap (does the proposed order push gross past the limit?) and refuses anything when the kill switch is already active. Daily-loss and drawdown breaches do not directly reject orders through `check_order`; they trip the switch through `update`, which then makes every subsequent `check_order` reject everything.

## Reading the aggregate

`snapshot()` returns a `PortfolioSnapshot` with totals, every per-strategy row (deep-copied so the caller can hold it past the snapshot lock), the current peak equity, the active drawdown, and the list of currently breached rules.

```python
snap = aggregator.snapshot()
print(snap.to_dict())
# {
#   "total_realized_pnl": ...,
#   "total_daily_pnl": ...,
#   "total_gross_exposure": ...,
#   "drawdown_pct": ...,
#   "kill_switch_active": ...,
#   "breaches": [{"rule": "...", "value": ..., "limit": ..., "detail": "..."}, ...],
#   "accounts": [...],
# }
```

The `to_dict()` method is JSON-serializable, so you can drop it straight into the runtime-state snapshot file the MCP analytics tools read. If you do, `get_pnl` and `get_kill_switch` over MCP automatically reflect portfolio-level state.

## After a breach

`reset_kill_switch()` is the explicit operator action that re-enables trading. The aggregator does not auto-reset; that is intentional. The next `update` re-evaluates and re-trips the switch if the underlying cause (drawdown, gross exposure, concentration) is still present, so resetting without addressing the cause buys you nothing. Read the breach detail, decide whether to flatten manually, and only then reset.

## Thread safety

Every public method takes the aggregator's internal lock. Update from inside `on_fill` callbacks, `check_order` from inside `RiskManager.preTradeCheck`, and `snapshot` from a background reporter thread; none of those need extra synchronization on the call site.

The optional `on_breach` callback fires inside the aggregator's lock to keep the breach state consistent across observers. Keep your callback cheap. If you want to do heavy work on a breach (page the operator, open a ticket), enqueue and process on a separate thread.

## What's not here

- **Multi-process aggregation.** Phase 1 is single-process. Phase 2 will plug in shared state (Redis, file-locked JSON, or a small RPC) without changing the public API.
- **Per-symbol concentration limits.** The current concentration rule is per-strategy. Per-symbol is a sensible follow-up when a real user hits the limitation.
- **Time-window PnL.** The aggregator carries the values you push in. If you want rolling 1-hour or week-to-date PnL, compute it in your update loop and feed it through one of the existing fields.

## See also

* [Inspect a running engine over MCP](mcp-runtime-inspection.md). Pair the aggregator's `snapshot().to_dict()` with the MCP analytics tools to expose portfolio-level state to an AI agent.
* [Live engine analytics over MCP](mcp-live-analytics.md). When a breach fires, `explain_decision` against the event log answers "what triggered the kill switch".
