# Market Data Reference

Events, buses, and order books.

## Events

### TradeEvent

**Header:** `flox/book/events/trade_event.h`

Represents a single trade execution.

```cpp
struct TradeEvent
{
  using Listener = IMarketDataSubscriber;

  Trade trade{};                // Core trade data
  int64_t seq = 0;              // Exchange sequence
  uint64_t trade_id = 0;        // Exchange trade ID
  uint64_t tickSequence = 0;    // Internal bus sequence

  MonoNanos recvNs{0};          // System receive time (monotonic)
  MonoNanos publishTsNs{0};     // Bus publish time (monotonic)
  UnixNanos exchangeMsgTsNs{0}; // Exchange message timestamp
};

struct Trade
{
  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  Price price{};
  Quantity quantity{};
  bool isBuy{false};
  UnixNanos exchangeTsNs{0};  // Exchange trade timestamp
};
```

### BookUpdateEvent

**Header:** `flox/book/events/book_update_event.h`

Represents an order book snapshot or delta update.

```cpp
struct BookUpdateEvent : public pool::PoolableBase<BookUpdateEvent>
{
  using Listener = IMarketDataSubscriber;

  BookUpdate update;            // Core book data
  int64_t seq{0};               // Exchange sequence
  int64_t prevSeq{0};           // Previous sequence (for gap detection)
  uint64_t tickSequence = 0;    // Internal bus sequence

  MonoNanos recvNs{0};          // System receive time
  MonoNanos publishTsNs{0};     // Bus publish time

  // Constructor requires PMR allocator
  BookUpdateEvent(std::pmr::memory_resource* res);
  void clear();  // Reset for pool reuse
};

struct BookUpdate
{
  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  BookUpdateType type{};        // SNAPSHOT or DELTA
  std::pmr::vector<BookLevel> bids;
  std::pmr::vector<BookLevel> asks;
  UnixNanos exchangeTsNs{0};
  UnixNanos systemTsNs{0};

  // Options fields
  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;

  BookUpdate(std::pmr::memory_resource* res);
};

struct BookLevel
{
  Price price{};
  Quantity quantity{};
};

enum class BookUpdateType { SNAPSHOT, DELTA };
```

### CandleEvent

**Header:** `flox/aggregator/events/candle_event.h`

Represents an OHLCV candlestick.

```cpp
struct CandleEvent
{
  using Listener = IMarketDataSubscriber;

  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  Candle candle{};
  uint64_t tickSequence = 0;  // Internal bus sequence
};

struct Candle
{
  Price open;
  Price high;
  Price low;
  Price close;
  Volume volume;
  TimePoint startTime;
  TimePoint endTime;
};
```

---

## Event Buses

All buses use the Disruptor pattern. See [The Disruptor Pattern](../explanation/disruptor.md).

### TradeBus

**Header:** `flox/book/bus/trade_bus.h`

```cpp
using TradeBus = EventBus<TradeEvent>;
```

Events are stored directly in the ring buffer (no pooling).

### BookUpdateBus

**Header:** `flox/book/bus/book_update_bus.h`

```cpp
using BookUpdateBus = EventBus<pool::Handle<BookUpdateEvent>>;
```

Uses `pool::Handle` because `BookUpdateEvent` contains variable-size vectors.

**Usage:**

```cpp
pool::Pool<BookUpdateEvent, 128> bookPool;

if (auto handle = bookPool.acquire()) {
  (*handle)->update.symbol = symbolId;
  (*handle)->update.type = BookUpdateType::SNAPSHOT;
  (*handle)->update.bids = {...};
  (*handle)->update.asks = {...};

  bookBus.publish(std::move(handle));
}
```

### CandleBus

**Header:** `flox/aggregator/bus/candle_bus.h`

```cpp
using CandleBus = EventBus<CandleEvent>;
```

### Common Bus Operations

```cpp
// Subscribe
bus.subscribe(listener, /*required=*/true);

// Start (spawns consumer threads)
bus.start();

// Publish
bus.publish(event);  // or bus.publish(std::move(handle))

// Wait for consumers
bus.waitConsumed(sequenceNumber);
bus.flush();  // Wait for all published events

// Stop
bus.stop();
```

---

## Order Books

### NLevelOrderBook

**Header:** `flox/book/nlevel_order_book.h`

Maintains a local order book from updates.

```cpp
template <size_t N = 10>
class NLevelOrderBook
{
public:
  explicit NLevelOrderBook(Price tickSize);

  void applyBookUpdate(const BookUpdateEvent& ev);

  Price bestBid() const;
  Price bestAsk() const;
  Quantity bidQty(size_t level) const;
  Quantity askQty(size_t level) const;

  Price midPrice() const;
  Price spread() const;

  void clear();
};
```

**Example:**

```cpp
NLevelOrderBook<5> book(Price::fromDouble(0.01));  // 5 levels, tick=0.01

void onBookUpdate(const BookUpdateEvent& ev) {
  book.applyBookUpdate(ev);

  auto mid = book.midPrice();
  auto spread = book.spread();
  // ...
}
```

---

## CandleAggregator

**Header:** `flox/aggregator/candle_aggregator.h`

Aggregates trades into OHLCV candles.

```cpp
class CandleAggregator : public IMarketDataSubscriber
{
public:
  CandleAggregator(std::chrono::seconds period, CandleBus* bus);

  void onTrade(const TradeEvent& ev) override;
  SubscriberId id() const override;
};
```

**Example:**

```cpp
auto candleBus = std::make_unique<CandleBus>();
auto aggregator = std::make_unique<CandleAggregator>(
    std::chrono::seconds{60},  // 1-minute candles
    candleBus.get()
);

tradeBus->subscribe(aggregator.get());
```

---

## Time Types

**Header:** `flox/util/base/time.h`

```cpp
using UnixNanos = int64_t;   // Nanoseconds since Unix epoch
using MonoNanos = int64_t;   // Nanoseconds from monotonic clock
using TimePoint = std::chrono::system_clock::time_point;
```

Helper functions:

```cpp
UnixNanos nowNsWallclock();      // Current wall-clock time
MonoNanos nowNsMonotonic();      // Current monotonic time
void init_timebase_mapping();    // Initialize time base (call once at startup)
```

---

## See Also

- [Engine & Lifecycle](engine.md) — Subscriber interfaces
- [The Disruptor Pattern](../explanation/disruptor.md) — How buses work
- [Memory Model](../explanation/memory-model.md) — Event pooling
