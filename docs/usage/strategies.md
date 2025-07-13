# Writing Strategies

Strategies in Flox are implemented by subclassing `IStrategy`, which defines a uniform interface for receiving market data and submitting orders. Strategies are market data subscribers with execution capability and injected dependencies.

## Purpose

Encapsulate trading logic that reacts to market data and interacts with execution and control systems.

## Interface Overview

```cpp
class IStrategy : public IMarketDataSubscriber {
 public:
  // Lifecycle
  virtual void onStart();
  virtual void onStop();

  // Market data callbacks
  virtual void onCandle(const CandleEvent& candle) override;
  virtual void onTrade(const TradeEvent& trade) override;
  virtual void onBookUpdate(const BookUpdateEvent& bookUpdate) override;

  // Identification and mode
  SubscriberId id() const override;
  SubscriberMode mode() const override;
};
```

## Strategy Lifecycle

* `onStart()` — called by the engine at startup before any events
* `onStop()` — called before engine shutdown

## Market Data

Strategies receive `BookUpdateEvent`, `TradeEvent`, and `CandleEvent` through their respective callbacks, depending on subscription mode.

## Execution and Control

All dependencies (e.g., order executor, validator, risk manager) must be owned or held directly by the strategy implementation. There are no getters or internal indirection in the base class. Example:

```cpp
class MyStrategy : public IStrategy {
public:
  MyStrategy(IOrderExecutor* executor,
             IRiskManager* risk,
             IOrderValidator* validator)
    : _executor(executor), _risk(risk), _validator(validator) {}

  void onBookUpdate(const BookUpdateEvent& update) override {
    if (!shouldEnter(update)) return;

    Order order = buildOrder(update);

    std::string reason;
    if (_validator && !_validator->validate(order, reason)) return;
    if (_risk && !_risk->allow(order)) return;

    _executor->submitOrder(order);
  }

private:
  IOrderExecutor* _executor;
  IRiskManager* _risk;
  IOrderValidator* _validator;

  bool shouldEnter(const BookUpdateEvent& update) const {
    return (update.bestAskPrice() - update.bestBidPrice()) >= MinSpread;
  }

  Order buildOrder(const BookUpdateEvent& update) const {
    return Order{
      .symbol = update.symbol(),
      .side = Side::BUY,
      .price = update.bestBidPrice() + TickImprovement,
      .quantity = DefaultQuantity,
      .type = OrderType::LIMIT
    };
  }

  static constexpr Price TickImprovement = 0.01;
  static constexpr Price MinSpread = 0.03;
  static constexpr Quantity DefaultQuantity = 100;
};
```

## Best Practices

* Keep callbacks non-blocking
* Never retain raw event pointers
* Avoid unnecessary dependencies
* Own or store all dependencies explicitly in the strategy

## Integration

Strategies are wired with their dependencies in the engine builder or main application, and subscribed to the relevant buses. PULL or PUSH mode is selectable via `mode()` override.
