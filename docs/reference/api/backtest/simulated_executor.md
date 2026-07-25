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

  // Top-of-book accessors. Return 0 when the side is empty.
  Price bestBidPrice(SymbolId symbol) const;
  Price bestAskPrice(SymbolId symbol) const;
  Price bookMidPrice(SymbolId symbol) const;  // 0 when either side is empty

  CompositeOrderLogic& compositeLogic();
};
```

`start()` and `stop()` are empty overrides. `compositeLogic()` is not `noexcept`.

### Brackets

A single call submits the entry leg and arms a take-profit plus stop pair on entry fill. The simulator
runs the state machine: the first child to fill cancels the other, and cancelling the bracket cancels
every still-live leg. Order ids are derived from the bracket id — `entry = bracketId * 3 + 0`,
`tp = bracketId * 3 + 1`, `stop = bracketId * 3 + 2`.

```cpp
void submitBracket(const BracketOrder& bracket);
void cancelBracket(uint64_t bracketId);
BracketStatus bracketStatus(uint64_t bracketId) const;

enum class BracketArmMode : uint8_t { OnFullFill = 0, OnPartialFill = 1 };
void setBracketChildArmMode(BracketArmMode mode) noexcept;
BracketArmMode bracketChildArmMode() const noexcept;
```

`OnFullFill` (the default) arms the children once, at full entry fill. `OnPartialFill` arms them at the
running entry-fill quantity on every partial, resizing via replace as more entry quantity fills.

### Iceberg

Applies to orders of `OrderType::ICEBERG`.

```cpp
void setIcebergRefreshLatency(int64_t latencyNs) noexcept;
int64_t icebergRefreshLatencyNs() const noexcept;

void setIcebergSizeRandomisationPct(double pct) noexcept;
double icebergSizeRandomisationPct() const noexcept;
void setIcebergJitterSeed(uint64_t seed) noexcept;

enum class IcebergPriorityMode : uint8_t { Back = 0, Retain = 1 };
void setIcebergPriorityMode(IcebergPriorityMode mode) noexcept;
IcebergPriorityMode icebergPriorityMode() const noexcept;
void setIcebergPriorityModeByName(const std::string& name) noexcept;

int64_t icebergHiddenRemainingRaw(OrderId id) const;  // 0 if not an iceberg
```

| Setting | Description |
|---------|-------------|
| `setIcebergRefreshLatency` | Delay between a visible tranche filling and the next being exposed. Applies to orders submitted after the call |
| `setIcebergSizeRandomisationPct` | Per-refresh visible-slice jitter as a fraction. `0.0` is deterministic, `0.10` is +/-10% uniform. Sampled from an internal RNG |
| `setIcebergJitterSeed` | Reseed that RNG to reproduce a specific draw sequence |
| `setIcebergPriorityMode` | `Back`: the refreshed slice goes to the back of the queue (most crypto venues). `Retain`: it keeps the prior slice's queue position (CME options, some Eurex contracts) |
| `icebergHiddenRemainingRaw` | Diagnostic: remaining hidden quantity, or 0 |

### Self-trade prevention

STP keys on `Order::accountId` plus an optional STP group. Two orders share an STP scope when their
`accountId`s are equal, or when both accounts map to the same non-zero group.

```cpp
void setSTPMode(STPMode mode) noexcept;
STPMode stpMode() const noexcept;

void setSTPGroupMembership(uint64_t accountId, uint64_t groupId);  // groupId 0 removes
uint64_t stpGroupFor(uint64_t accountId) const;
bool sameStpScope(uint64_t a, uint64_t b) const;
```

See [`STPMode`](../common.md#stpmode) for the modes.

### FOK semantics

```cpp
enum class FokMode : uint8_t { AnyPrice = 0, SinglePrice = 1 };
void setFokMode(FokMode mode) noexcept;
FokMode fokMode() const noexcept;
void setFokModeByName(const std::string& name);  // "any_price" | "single_price"
```

`AnyPrice` (the default, matching crypto venues) fills when cumulative liquidity at prices crossing
the order's limit is at least the order quantity. `SinglePrice` (CME, Eurex, most US equities) fills
only when the level at the limit price holds the whole quantity in one trade. The simulator currently
consults top-of-book quantity only. `setFokModeByName` is case-insensitive and ignores unknown values.

### Latency

```cpp
void setSubmitAckLatency(int64_t latencyNs, int64_t jitterNs);
void setCancelAckLatency(int64_t latencyNs, int64_t jitterNs);
void setReplaceAckLatency(int64_t latencyNs, int64_t jitterNs);

void setSubmitAckLatencyDistribution(const LatencyDistribution& dist);
void setCancelAckLatencyDistribution(const LatencyDistribution& dist);
void setReplaceAckLatencyDistribution(const LatencyDistribution& dist);

void applyLatencyProfile(const char* name);
```

The scalar setters delegate to the distribution setters: zero jitter becomes `Constant`, non-zero
becomes `Uniform` over `[base - jitter, base + jitter]`.

### Venue availability and rate limits

```cpp
void setVenueAvailability(VenueAvailability* availability);  // nullptr disables
VenueAvailability* venueAvailability() noexcept;

void setRateLimitPolicy(const RateLimitPolicy& policy);
void clearRateLimitPolicy();
bool hasRateLimitPolicy() const noexcept;
RateLimitPolicy& rateLimitPolicy();
```

Submit, cancel and replace issued while the venue is down are buffered and flushed at the recovery
edge in FIFO order. Market-data callbacks (`onTrade`, `onBookUpdate`, `onBar`) are silently dropped
during an outage, so the strategy sees a feed gap.

Submit, cancel and replace consult the rate-limit policy first; an overflow emits
`OrderEventStatus::REJECTED_RATE_LIMIT` and the action is not committed.

### Queue tuning

`setQueueFifoTopN`, `setTopPriorityShare`, `setLmmOrders`, `setLmmBonusMultiplier`,
`setOrderPriorityMultiplier` and `setQueuePositionMinChangeFraction` are documented in
[Queue simulation](queue_simulation.md).

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

Per-symbol best bid, best ask, last trade and level quantities are updated via `onBookUpdate()` and
`onTrade()`. The state struct itself is **private**; read it through the public accessors:

```cpp
Price bestBidPrice(SymbolId symbol) const;   // 0 when the side is empty
Price bestAskPrice(SymbolId symbol) const;
Price bookMidPrice(SymbolId symbol) const;   // 0 when either side is empty
```

The tracked level quantities are consumed internally by `VOLUME_IMPACT` slippage and by the queue
tracker; they are not exposed.

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
