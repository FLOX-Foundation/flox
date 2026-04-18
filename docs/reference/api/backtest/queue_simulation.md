# Queue simulation

Limit orders in a real market do not fill the instant the book touches their price. They wait behind earlier orders at the same level and fill only after an incoming aggressive order consumes the queue ahead. The backtest engine offers a queue simulator that models this behavior.

## Modes

```cpp
enum class QueueModel : uint8_t { NONE, TOB, FULL };
```

| Mode | Behavior |
|------|----------|
| `NONE` | Legacy MVP behavior. A limit order fills immediately when the book crosses its price. Lowest overhead. Backward-compatible. |
| `TOB` | Tracks queue position at the top-of-book level where the order was placed. Trades at the level consume queue-ahead first, then fill the order (partial fills supported). Cancels in front shrink the queue-ahead value proportionally. |
| `FULL` | Tracks queue position at up to `queueDepth` price levels per side. Useful for strategies that place resting orders a few ticks inside the book. |

## Heuristics

`TOB` and `FULL` both use the well-known trade-ahead heuristic:

1. A trade event at the order's price first consumes `aheadRemaining` for each waiting order at that level.
2. Remaining trade volume fills the order (partial fills produce `PARTIALLY_FILLED` events).
3. If the level quantity shrinks without a corresponding trade, the shrink is interpreted as cancels from orders in the queue and reduces `aheadRemaining` proportionally.
4. Growth of the level places new liquidity behind the order and does not move it.

## Configuration

```cpp
BacktestConfig cfg;
cfg.queueModel = QueueModel::TOB;

BacktestRunner runner(cfg);
```

Direct executor control:

```cpp
SimulatedExecutor exec(clock);
exec.setQueueModel(QueueModel::TOB, /*depth=*/1);
```

## What you need to feed

Queue simulation requires trade quantities. Use the overload `onTrade(symbol, price, qty, isBuy)` or, when going through `BacktestRunner`, let the replay stream drive trade events with their real quantities. The older `onTrade(symbol, price, isBuy)` overload keeps working for backward compatibility but does not drive queue fills.

In Python: call `executor.on_trade_qty(symbol, price, quantity, is_buy)`.

In C API: `flox_executor_on_trade_qty(executor, symbol, price, quantity, is_buy)`.

In JavaScript: `executor.onTradeQty(symbol, price, quantity, isBuy)`.

## Caveats

- When no trade events flow in, queued orders never fill. That is faithful to the market: without executions no one consumes the queue.
- Orders placed away from the tracked levels fall back to `NONE` behavior.
- The `FULL` mode's behavior beyond `queueDepth` levels is the same as `NONE`.
