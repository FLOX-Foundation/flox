# SimulatedExecutor

`SimulatedExecutor` simulates order execution against historical market data for backtesting. It supports slippage models on market-style fills and optional queue simulation for resting limit orders.

```cpp
class SimulatedExecutor : public IOrderExecutor
{
public:
  static constexpr size_t kMaxSymbols = 256;
  static constexpr size_t kDefaultOrderCapacity = 64;
  static constexpr size_t kDefaultFillCapacity = 4096;

  using OrderEventCallback = std::function<void(const OrderEvent&)>;

  explicit SimulatedExecutor(IClock& clock);

  void setOrderEventCallback(OrderEventCallback cb);

  // Apply slippage and queue-simulation settings from a BacktestConfig.
  void applyConfig(const BacktestConfig& config);

  // Convenience setters for callers without a full BacktestConfig.
  void setDefaultSlippage(const SlippageProfile& profile);
  void setSymbolSlippage(SymbolId symbol, const SlippageProfile& profile);
  void setQueueModel(QueueModel model, size_t depth);

  void start() override;
  void stop() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void cancelAllOrders(SymbolId symbol) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;
  void submitOCO(const OCOParams& params) override;

  ExchangeCapabilities capabilities() const override;

  void onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                    const std::pmr::vector<BookLevel>& asks);
  void onTrade(SymbolId symbol, Price price, bool isBuy);
  void onTrade(SymbolId symbol, Price price, Quantity qty, bool isBuy);
  void onBar(SymbolId symbol, Price close);

  const std::vector<Fill>& fills() const;
  std::vector<Fill> extractFills();
  const std::vector<Order>& conditionalOrders() const;

  CompositeOrderLogic& compositeLogic() noexcept;
};
```

## Execution logic

### Market orders

| Side | Fill price |
|------|-----------|
| BUY  | Best ask (or last trade if no book) |
| SELL | Best bid (or last trade if no book) |

Slippage is applied to the fill price based on the active `SlippageProfile`. See [Slippage](./slippage.md) for the available models.

### Limit orders

Without queue simulation (`QueueModel::NONE`, the default), a limit order fills as soon as the book crosses its price:

| Side | Condition to fill |
|------|-------------------|
| BUY  | `orderPrice >= bestAsk` |
| SELL | `orderPrice <= bestBid` |

With queue simulation enabled, non-crossing limits are registered in an `OrderQueueTracker` and fill only when trades at the level consume the queue ahead of them. Crossing ("marketable") limits still fill immediately at the best price without slippage. See [Queue simulation](./queue_simulation.md).

### Conditional orders

Stop, take-profit, and trailing-stop orders are stored separately and checked on each market update.

| Type | Trigger condition |
|------|-------------------|
| STOP_MARKET / STOP_LIMIT | SELL: price ≤ trigger, BUY: price ≥ trigger |
| TAKE_PROFIT_MARKET / TAKE_PROFIT_LIMIT | SELL: price ≥ trigger, BUY: price ≤ trigger |
| TRAILING_STOP | Trigger follows price; executes on reversal |

When triggered, conditional orders convert to market or limit and execute through the usual path (slippage applies to the market leg).

### OCO orders

```cpp
OCOParams params;
params.order1 = orderA;
params.order2 = orderB;
executor.submitOCO(params);
```

When one order fills, the other is canceled automatically.

### Trailing stop

```cpp
struct TrailingState
{
  Price activationPrice{};  // price when trailing stop was activated
  Price currentTrigger{};   // current trigger price (moves with price)
};
```

SELL trailing: trigger follows price up (never down). BUY trailing: trigger follows price down (never up).

## Feeding market data

| Call | When to use |
|------|-------------|
| `onBookUpdate(symbol, bids, asks)` | Full L2 snapshot. Updates top-of-book state and drives the queue tracker's level-update heuristic. |
| `onTrade(symbol, price, isBuy)` | Trade event without quantity. Keeps legacy behavior but does **not** drive queue-simulated fills. |
| `onTrade(symbol, price, qty, isBuy)` | Trade event with quantity. Required for queue simulation. |
| `onBar(symbol, close)` | Bar close shortcut. Sets best bid, best ask, and last trade to the close price. |

## Order events

| Event | When |
|-------|------|
| `SUBMITTED` | Order received |
| `ACCEPTED` | Order validated |
| `PENDING_TRIGGER` | Conditional order waiting for trigger |
| `TRIGGERED` | Conditional order triggered |
| `FILLED` | Fully executed |
| `PARTIALLY_FILLED` | Partial execution (common with queue simulation) |
| `CANCELED` | Order canceled |
| `REPLACED` | Order modified |

Trailing stop updates emit `TRAILING_UPDATED` events with the new trigger price.

## Market state

Per-symbol state updated via `onBookUpdate()` and `onTrade()`:

```cpp
struct MarketState
{
  int64_t bestBidRaw{0};
  int64_t bestAskRaw{0};
  int64_t lastTradeRaw{0};
  int64_t bestBidQtyRaw{0};
  int64_t bestAskQtyRaw{0};
  bool hasBid{false};
  bool hasAsk{false};
  bool hasTrade{false};
};
```

`bestBidQtyRaw` and `bestAskQtyRaw` are used by `VOLUME_IMPACT` slippage and by the queue tracker.

## Performance

- Fixed-size array for symbols 0-255 (fast path)
- Overflow vector for symbol IDs >= 256
- O(n) pending order scan on each market update
- Pre-allocated fill vector (default 4096)

## See also

- [BacktestRunner](./backtest_runner.md) — Run backtests with strategies
- [BacktestResult](./backtest_result.md) — Performance statistics
- [Slippage](./slippage.md)
- [Queue simulation](./queue_simulation.md)
