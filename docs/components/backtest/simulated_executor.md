# SimulatedExecutor

`SimulatedExecutor` simulates order execution against historical market data for backtesting.

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

  void start() override;
  void stop() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void cancelAllOrders(SymbolId symbol) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  // OCO: one-cancels-other
  void submitOCO(const OCOParams& params) override;

  ExchangeCapabilities capabilities() const override;

  void onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                    const std::pmr::vector<BookLevel>& asks);
  void onTrade(SymbolId symbol, Price price, bool isBuy);

  const std::vector<Fill>& fills() const noexcept;
  const std::vector<Order>& conditionalOrders() const noexcept;

  CompositeOrderLogic& compositeLogic() noexcept;
};
```

## Execution Logic

### Market Orders

| Side | Fill Price |
|------|------------|
| BUY | Best ask (or last trade if no book) |
| SELL | Best bid (or last trade if no book) |

### Limit Orders

| Side | Condition to Fill |
|------|-------------------|
| BUY | `orderPrice >= bestAsk` |
| SELL | `orderPrice <= bestBid` |

Unfilled limit orders stay pending until price crosses or canceled.

### Conditional Orders

Conditional orders (stop, take-profit, trailing stop) are stored separately and checked on each market update.

| Type | Trigger Condition |
|------|-------------------|
| STOP_MARKET / STOP_LIMIT | SELL: price ≤ trigger, BUY: price ≥ trigger |
| TAKE_PROFIT_MARKET / TAKE_PROFIT_LIMIT | SELL: price ≥ trigger, BUY: price ≤ trigger |
| TRAILING_STOP | Trigger follows price, executes on reversal |

When triggered, conditional orders convert to market/limit and execute.

### OCO Orders

One-Cancels-Other orders are managed via `CompositeOrderLogic`:

```cpp
OCOParams params;
params.stopOrder = stopOrder;
params.takeProfitOrder = tpOrder;
executor.submitOCO(params);
```

When one order fills, the other is automatically canceled.

### Trailing Stop

Trailing stops track price movement:

```cpp
struct TrailingState
{
  Price activationPrice{};  // price when trailing stop was activated
  Price currentTrigger{};   // current trigger price (moves with price)
};
```

- **SELL trailing**: trigger follows price UP (never down)
- **BUY trailing**: trigger follows price DOWN (never up)

## Order Events

Emitted via callback:

| Event | When |
|-------|------|
| `SUBMITTED` | Order received |
| `ACCEPTED` | Order validated |
| `PENDING_TRIGGER` | Conditional order waiting for trigger |
| `TRIGGERED` | Conditional order triggered |
| `FILLED` | Fully executed |
| `PARTIALLY_FILLED` | Partial execution |
| `CANCELED` | Order canceled |
| `REPLACED` | Order modified |

Trailing stop updates emit `TRAILING_UPDATE` events with new trigger price.

## Exchange Capabilities

```cpp
ExchangeCapabilities capabilities() const override {
  return ExchangeCapabilities::simulated();
}
```

Returns capabilities indicating support for:

- All order types (limit, market, stop, take-profit, trailing)
- OCO orders
- Replace orders
- Cancel all orders

## Market State

Per-symbol state updated via `onBookUpdate()` and `onTrade()`:

```cpp
struct MarketState
{
  int64_t bestBidRaw{0};
  int64_t bestAskRaw{0};
  int64_t lastTradeRaw{0};
  bool hasBid{false};
  bool hasAsk{false};
  bool hasTrade{false};
};
```

## Performance

- Fixed-size array for symbols 0-255 (fast path)
- Overflow vector for symbol IDs >= 256
- O(n) pending order scan on each market update
- Pre-allocated fill vector (default 4096)

## See Also

- [BacktestRunner](./backtest_runner.md) — Run backtests with strategies
- [BacktestResult](./backtest_result.md) — Performance statistics
- [Order Types](../../reference/api/common.md#ordertype) — Order type enum
- [ExchangeCapabilities](../../reference/api/execution/exchange_capabilities.md) — Feature discovery
