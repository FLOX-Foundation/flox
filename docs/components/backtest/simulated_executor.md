# SimulatedExecutor

`SimulatedExecutor` simulates order execution against historical market data.

```cpp
class SimulatedExecutor : public IOrderExecutor
{
public:
  explicit SimulatedExecutor(IClock& clock);

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void cancelAllOrders(SymbolId symbol) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  void onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                    const std::pmr::vector<BookLevel>& asks);
  void onTrade(SymbolId symbol, Price price, bool isBuy);

  void setOrderEventCallback(OrderEventCallback cb);
  const std::vector<Fill>& fills() const noexcept;
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

## Order Events

Emitted via callback:

| Event | When |
|-------|------|
| `SUBMITTED` | Order received |
| `ACCEPTED` | Order validated |
| `FILLED` | Fully executed |
| `PARTIALLY_FILLED` | Partial execution |
| `CANCELED` | Order canceled |
| `REPLACED` | Order modified |

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

* Fixed-size array for symbols 0-255 (fast path)
* Overflow vector for symbol IDs >= 256
* O(n) pending order scan on each market update
