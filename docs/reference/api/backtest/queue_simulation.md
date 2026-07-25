# Queue simulation

Limit orders in a real market do not fill the instant the book touches their price. They wait behind earlier orders at the same level and fill only after an incoming aggressive order consumes the queue ahead. The backtest engine offers a queue simulator that models this behavior.

## Modes

```cpp
enum class QueueModel : uint8_t {
  NONE,
  TOB,
  FULL,
  PRO_RATA,
  PRO_RATA_WITH_FIFO,
  TOP_PRO_LMM,
  PRO_RATA_WITH_PRIORITY,
};
```

| Mode | Behavior |
|------|----------|
| `NONE` | Legacy MVP behavior. A limit order fills immediately when the book crosses its price. Lowest overhead. Backward-compatible. |
| `TOB` | Tracks queue position at the top-of-book level where the order was placed. Trades at the level consume queue-ahead first, then fill the order (partial fills supported). Cancels in front shrink the queue-ahead value proportionally. |
| `FULL` | Tracks queue position at up to `queueDepth` price levels per side. Useful for strategies that place resting orders a few ticks inside the book. |
| `PRO_RATA` | Pure pro-rata. Every order at the price level receives a share of the trade proportional to its size. Models venues whose matching engine distributes a trade across all orders at the best price (most options venues, some hybrid spot venues). |
| `PRO_RATA_WITH_FIFO` | FIFO for the first N orders at the level, then pro-rata across the remainder. Models hybrid venues that reward queue front but distribute the rest. Configure N with `setQueueFifoTopN`. |
| `TOP_PRO_LMM` | CME TOP-PRO-LMM (Globex options). The queue-front order receives a fixed share of each incoming trade, capped by its remaining size; the remainder distributes pro-rata with a bonus multiplier for Lead Market Maker orders. Configure with `setTopPriorityShare`, `setLmmOrders`, `setLmmBonusMultiplier`, `setOrderPriorityMultiplier`. |
| `PRO_RATA_WITH_PRIORITY` | ICE-style size-pro-rata with a priority multiplier. Every order at the level gets effective weight `remaining × priorityMultiplier`, distributed proportionally. The per-order multiplier defaults to 1.0; set it with `setOrderPriorityMultiplier`. |

Only `NONE`, `TOB`, `FULL`, `PRO_RATA` and `PRO_RATA_WITH_FIFO` have C-API constants
(`FloxQueueModel`); `TOP_PRO_LMM` and `PRO_RATA_WITH_PRIORITY` are C++ only.

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

Per-model tuning on `SimulatedExecutor`:

| Setter | Applies to | Description |
|--------|-----------|-------------|
| `setQueueModel(QueueModel, size_t depth)` | all | Select the model and the tracked depth |
| `setQueueFifoTopN(size_t topN)` | `PRO_RATA_WITH_FIFO` | First N orders at a level fill FIFO; the rest split pro-rata. Ignored by other models |
| `setTopPriorityShare(double share)` | `TOP_PRO_LMM` | Fraction of each trade reserved for the queue-front order. Default 0.40 |
| `setLmmOrders(const std::vector<OrderId>&)` | `TOP_PRO_LMM` | Mark these orders as Lead Market Makers |
| `setLmmBonusMultiplier(double)` | `TOP_PRO_LMM` | LMM bonus applied during the pro-rata remainder |
| `setOrderPriorityMultiplier(OrderId, double)` | `TOP_PRO_LMM`, `PRO_RATA_WITH_PRIORITY` | Per-order weight, default 1.0. Effective weight is `remaining × multiplier` |
| `setQueuePositionMinChangeFraction(double)` | `TOB`, `FULL` | Minimum relative move before a `QUEUE_POSITION_UPDATED` event is emitted |

## What you need to feed

Queue simulation requires trade quantities. Use the overload `onTrade(symbol, price, qty, isBuy)` or, when going through `BacktestRunner`, let the replay stream drive trade events with their real quantities. The older `onTrade(symbol, price, isBuy)` overload keeps working for backward compatibility but does not drive queue fills.

In Python: call `executor.on_trade_qty(symbol, price, quantity, is_buy)`.

In C API: `flox_simulated_executor_on_trade_qty(executor, symbol, price, quantity, is_buy)`.

In JavaScript: `executor.onTradeQty(symbol, price, quantity, isBuy)`.

## Caveats

- When no trade events flow in, queued orders never fill. That is faithful to the market: without executions no one consumes the queue.
- Orders placed away from the tracked levels fall back to `NONE` behavior.
- The `FULL` mode's behavior beyond `queueDepth` levels is the same as `NONE`.
