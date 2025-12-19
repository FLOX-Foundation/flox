# SignalStrategy

`SignalStrategy` is a base class for strategies that emit trading signals instead of calling executor directly.

## Signal

```cpp
enum class SignalType : uint8_t
{
  Market,
  Limit,
  Cancel,
  CancelAll
};

struct Signal
{
  SignalType type;
  SymbolId symbol;
  Side side;
  Price price;
  Quantity quantity;
  OrderId orderId;

  static Signal marketBuy(SymbolId sym, Quantity qty);
  static Signal marketSell(SymbolId sym, Quantity qty);
  static Signal limitBuy(SymbolId sym, Price px, Quantity qty);
  static Signal limitSell(SymbolId sym, Price px, Quantity qty);
  static Signal cancel(OrderId id);
  static Signal cancelAll(SymbolId sym);
};
```

## SignalStrategy

```cpp
class SignalStrategy : public IStrategy
{
public:
  void setSignalHandler(ISignalHandler* handler) noexcept;

protected:
  void emit(const Signal& signal);

  void emitMarketBuy(SymbolId symbol, Quantity qty);
  void emitMarketSell(SymbolId symbol, Quantity qty);
  void emitLimitBuy(SymbolId symbol, Price price, Quantity qty);
  void emitLimitSell(SymbolId symbol, Price price, Quantity qty);
  void emitCancel(OrderId orderId);
  void emitCancelAll(SymbolId symbol);
};
```

## ISignalHandler

```cpp
class ISignalHandler
{
public:
  virtual void onSignal(const Signal& signal) = 0;
};
```

`BacktestRunner` implements `ISignalHandler` and converts signals to orders.

## Usage

```cpp
class MyStrategy : public SignalStrategy
{
public:
  SubscriberId id() const override { return 1; }
  void start() override { _running = true; }
  void stop() override { _running = false; }

  void onTrade(const TradeEvent& ev) override
  {
    if (!_running) return;

    // Strategy logic
    if (shouldBuy())
      emitMarketBuy(ev.trade.symbol, Quantity::fromDouble(1.0));
    else if (shouldSell())
      emitMarketSell(ev.trade.symbol, Quantity::fromDouble(1.0));
  }

private:
  bool _running{false};
};

// With BacktestRunner
BacktestRunner runner(config);
MyStrategy strategy;
runner.setSignalStrategy(&strategy);  // auto-connects handler
```

## Why Signals

| Direct Executor | Signals |
|-----------------|---------|
| `_executor.submitOrder(order)` | `emitMarketBuy(symbol, qty)` |
| Strategy knows about executor | Strategy decoupled from execution |
| Harder to test | Easy to mock signal handler |
| No interception point | Risk management can intercept |
