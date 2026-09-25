# IExchangeConnector

`IExchangeConnector` is the abstract interface for real-time market data adapters. It provides lifecycle control, typed callback delivery for market data events, and error notifications.

```cpp
class IExchangeConnector : public ISubsystem, public IDrainable
{
public:
  using BookUpdateCallback = MoveOnlyFunction<void(const BookUpdateEvent&)>;
  using TradeCallback = MoveOnlyFunction<void(const TradeEvent&)>;
  using DisconnectCallback = MoveOnlyFunction<void(std::string_view reason)>;
  using SequenceGapCallback = MoveOnlyFunction<void(uint64_t expected, uint64_t received)>;
  using StaleDataCallback = MoveOnlyFunction<void(SymbolId symbol, uint64_t lastUpdateMs)>;

  virtual ~IExchangeConnector() = default;

  bool drain(uint32_t timeoutMs) override { return true; }

  virtual std::string exchangeId() const = 0;

  virtual void setCallbacks(BookUpdateCallback onBookUpdate, TradeCallback onTrade);
  virtual void setErrorCallbacks(DisconnectCallback onDisconnect,
                                 SequenceGapCallback onSequenceGap,
                                 StaleDataCallback onStaleData);

  virtual void pollFeedHealth(MonoNanos now);

protected:
  void emitBookUpdate(const BookUpdateEvent& bu);
  void emitTrade(const TradeEvent& t);
  void emitDisconnect(std::string_view reason);
  void emitSequenceGap(uint64_t expected, uint64_t received);
  void emitStaleData(SymbolId symbol, uint64_t lastUpdateMs);

  void markFeedActivity(SymbolId symbol, MonoNanos when);
  void checkStaleFeeds(MonoNanos now, int timeoutMs);
};
```

## Purpose

* Abstract base for all exchange-specific connectors (e.g. Bybit, Mock, Replay), handling event emission and lifecycle.

## Responsibilities

| Aspect          | Details                                                                 |
| --------------- | ----------------------------------------------------------------------- |
| Lifecycle       | Inherits `start()` and `stop()` from `ISubsystem`.                      |
| Draining        | Implements `IDrainable` for graceful shutdown with pending operations.  |
| Identity        | `exchangeId()` provides a stable identifier for the connector instance. |
| Data Callbacks  | `setCallbacks()` binds handlers for book and trade events.              |
| Error Callbacks | `setErrorCallbacks()` binds handlers for connection/data errors.        |
| Event Routing   | `emit*()` methods dispatch data and errors to subscribers.              |
| Feed Health     | `markFeedActivity()` stamps per-symbol arrivals; `pollFeedHealth()` is driven by the caller and forwards to `checkStaleFeeds()`. |

## Error Callbacks

| Callback       | When Called                                                    |
| -------------- | -------------------------------------------------------------- |
| onDisconnect   | Connection lost or closed unexpectedly.                        |
| onSequenceGap  | Sequence number gap detected (expected vs received).           |
| onStaleData    | No updates received for a symbol beyond threshold.             |

`onSequenceGap` and `onStaleData` are both raised off state the connector
keeps per symbol. Staleness is polled rather than timed — no connector owns a
timer thread — so the owner calls `pollFeedHealth(now)` on its own cadence;
the window comes from the venue config's `staleDataTimeoutMs`, where `0`
disables the check. A symbol is reported once per staleness episode and fresh
data re-arms it. See [Connectors](../../../explanation/connectors.md#feed-health)
for the contract every in-tree venue honours.

## Notes

* Inherits from `ISubsystem` and `IDrainable`, enabling unified lifecycle and graceful shutdown.
* Callbacks use `MoveOnlyFunction` to avoid `std::function` overhead and enable capturing closures with ownership.
* Implementations must call `emit*()` manually from internal processing (e.g. websocket handler).
* The class is intentionally non-copyable and non-thread-safe — connectors are expected to run in isolated threads.
