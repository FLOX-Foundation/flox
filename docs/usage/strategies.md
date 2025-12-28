# Writing Strategies

Strategies in FLOX are implemented by subclassing `IStrategy`, which defines a uniform interface for receiving market data and managing lifecycle. Strategies are market data subscribers with execution capability and injected dependencies.

## Purpose

Encapsulate trading logic that reacts to market data and interacts with execution and control systems.

## Interface Overview

```cpp
class IStrategy : public ISubsystem, public IMarketDataSubscriber
{
public:
  virtual ~IStrategy() = default;
};
```

`IStrategy` inherits from:

- `ISubsystem` — provides `start()` and `stop()` lifecycle hooks
- `IMarketDataSubscriber` — provides market data callbacks and `id()` for routing

## Market Data Callbacks

From `IMarketDataSubscriber`:

```cpp
virtual void onBookUpdate(const BookUpdateEvent& ev) {}
virtual void onTrade(const TradeEvent& ev) {}
virtual void onBar(const BarEvent& ev) {}
```

## Lifecycle

From `ISubsystem`:

```cpp
virtual void start() {}
virtual void stop() {}
```

## Example Strategy

```cpp
class MyStrategy : public IStrategy
{
public:
  MyStrategy(IOrderExecutor* executor, IRiskManager* risk, IOrderValidator* validator)
      : _executor(executor), _risk(risk), _validator(validator)
  {
  }

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void onBookUpdate(const BookUpdateEvent& update) override
  {
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

  bool shouldEnter(const BookUpdateEvent& update) const
  {
    // Example: enter when spread is wide enough
    return true;
  }

  Order buildOrder(const BookUpdateEvent& update) const
  {
    return Order{
        .symbol = update.symbol,
        .side = Side::BUY,
        .price = Price{100},
        .quantity = Quantity{10},
        .type = OrderType::LIMIT};
  }
};
```

## Best Practices

* Keep callbacks non-blocking
* Never retain raw event pointers
* Avoid unnecessary dependencies
* Own or store all dependencies explicitly in the strategy
* Implement `id()` to return a unique identifier (typically `reinterpret_cast<SubscriberId>(this)`)

## Integration

Strategies are wired with their dependencies in the engine builder or main application, and subscribed to the relevant buses:

```cpp
auto strategy = std::make_shared<MyStrategy>(executor, risk, validator);

bookUpdateBus->subscribe(strategy.get());
tradeBus->subscribe(strategy.get());

bookUpdateBus->start();
tradeBus->start();
```
