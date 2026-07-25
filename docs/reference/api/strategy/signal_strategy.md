# Signals

```cpp
#include "flox/strategy/signal.h"
#include "flox/strategy/abstract_signal_handler.h"
```

A `Signal` is the intent to act on an order — submit, cancel, modify, or provide liquidity — without
naming an executor. Strategies emit signals; a handler turns them into orders.

!!! note "There is no `SignalStrategy` class"
    Signal emission is built into [`flox::Strategy`](strategy.md), which owns the `emit*` helpers and
    implements `IStrategy::setSignalHandler`. Derive from `Strategy`, not from a separate signal base
    class.

## SignalType

```cpp
enum class SignalType : uint8_t
{
  Market,
  Limit,
  Cancel,
  CancelAll,
  Modify,
  StopMarket,
  StopLimit,
  TakeProfitMarket,
  TakeProfitLimit,
  TrailingStop,
  OCO,
  ProvideLiquidity,
  WithdrawLiquidity
};
```

`ProvideLiquidity` and `WithdrawLiquidity` are executed by the on-chain connector, not by the CEX or
backtest path; `symbol` identifies the pool.

## Signal

```cpp
struct Signal
{
  SignalType type{SignalType::Market};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderId orderId{};

  Price newPrice{};                 // Modify
  Quantity newQuantity{};           // Modify

  Price triggerPrice{};             // Stop / take-profit trigger
  Price trailingOffset{};           // Trailing stop, absolute
  int32_t trailingCallbackRate{0};  // Trailing stop, bps (100 = 1%)
  TimeInForce timeInForce{TimeInForce::GTC};
  bool reduceOnly{false};
  bool postOnly{false};

  OrderId linkedOrderId{};          // OCO

  Price priceLower{};               // ProvideLiquidity range
  Price priceUpper{};
  Quantity liquidity{};             // LP position size
};
```

| Field | Type | Used by |
|-------|------|---------|
| `type` | `SignalType` | all |
| `symbol` | `SymbolId` | all except `Cancel` and `Modify` |
| `side` | `Side` | entry types |
| `price` | `Price` | `Limit`, `StopLimit`, `TakeProfitLimit`, `OCO` |
| `quantity` | `Quantity` | entry types |
| `orderId` | `OrderId` | all |
| `newPrice`, `newQuantity` | `Price`, `Quantity` | `Modify` |
| `triggerPrice` | `Price` | `Stop*`, `TakeProfit*`; second price for `OCO` |
| `trailingOffset` | `Price` | `TrailingStop` (absolute) |
| `trailingCallbackRate` | `int32_t` | `TrailingStop` (bps) |
| `timeInForce` | `TimeInForce` | `Limit` |
| `reduceOnly`, `postOnly` | `bool` | modifiers |
| `linkedOrderId` | `OrderId` | `OCO` |
| `priceLower`, `priceUpper`, `liquidity` | `Price`, `Price`, `Quantity` | `ProvideLiquidity`, `WithdrawLiquidity` |

## Factories

Every factory that opens an order takes an explicit `OrderId` as its last argument — the caller owns
id allocation. `Strategy`'s `emit*` helpers allocate one and return it.

```cpp
static Signal marketBuy(SymbolId sym, Quantity qty, OrderId id);
static Signal marketSell(SymbolId sym, Quantity qty, OrderId id);
static Signal limitBuy(SymbolId sym, Price px, Quantity qty, OrderId id);
static Signal limitSell(SymbolId sym, Price px, Quantity qty, OrderId id);

static Signal cancel(OrderId id);
static Signal cancelAll(SymbolId sym);
static Signal modify(OrderId id, Price newPx, Quantity newQty);

static Signal stopMarket(SymbolId sym, Side side, Price trigger, Quantity qty, OrderId id);
static Signal stopLimit(SymbolId sym, Side side, Price trigger, Price limit,
                        Quantity qty, OrderId id);
static Signal takeProfitMarket(SymbolId sym, Side side, Price trigger, Quantity qty, OrderId id);
static Signal takeProfitLimit(SymbolId sym, Side side, Price trigger, Price limit,
                              Quantity qty, OrderId id);
static Signal trailingStop(SymbolId sym, Side side, Price offset, Quantity qty, OrderId id);
static Signal trailingStopPercent(SymbolId sym, Side side, int32_t callbackBps,
                                  Quantity qty, OrderId id);
static Signal oco(SymbolId sym, Side side, Price price1, Price price2, Quantity qty, OrderId id);

static Signal provideLiquidity(SymbolId pool, Price priceLower, Price priceUpper,
                               Quantity liquidity, OrderId id);
static Signal withdrawLiquidity(SymbolId pool, Quantity liquidity, OrderId id);
```

`oco()` stores the second price in `triggerPrice`; it does not populate `linkedOrderId`.

## Modifiers

Chainable, applied after construction:

```cpp
Signal& withTimeInForce(TimeInForce tif);
Signal& withReduceOnly(bool val = true);
Signal& withPostOnly(bool val = true);
```

## ISignalHandler

```cpp
class ISignalHandler
{
 public:
  virtual ~ISignalHandler() = default;
  virtual void onSignal(const Signal& signal) = 0;
};
```

`BacktestRunner` implements `ISignalHandler` and converts signals into orders for the simulated
executor (or a caller-supplied one set via `setExecutor`).

## Wiring

`IStrategy::setSignalHandler(ISignalHandler*)` connects the two. `BacktestRunner::setStrategy` does
it for you:

```cpp
#include "flox/backtest/backtest_runner.h"
#include "flox/strategy/strategy.h"

using namespace flox;

class MyStrategy : public Strategy
{
 public:
  MyStrategy(SubscriberId id, SymbolId symbol, const SymbolRegistry& registry)
      : Strategy(id, symbol, registry)
  {
  }

 protected:
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override
  {
    if (ctx.position.isZero())
    {
      emitMarketBuy(ev.trade.symbol, Quantity::fromDouble(1.0));
    }
  }

  void onSymbolFill(SymbolContext& ctx, const OrderEvent& ev) override
  {
    // React to the fill.
  }
};

SymbolRegistry registry;
SymbolId sym = registry.registerSymbol("binance", "BTCUSDT");

BacktestConfig config;
BacktestRunner runner(config);

MyStrategy strategy(1, sym, registry);
runner.setStrategy(&strategy);  // also installs the signal handler
```

`Strategy` constructors both require a `const SymbolRegistry&`; there is no registry-free overload.

## Why signals

| Direct executor | Signals |
|-----------------|---------|
| `_executor.submitOrder(order)` | `emitMarketBuy(symbol, qty)` |
| Strategy holds an executor pointer | Strategy decoupled from execution |
| Harder to test | Easy to substitute a mock handler |
| No interception point | Risk manager, validator and kill switch gate the signal |

See [Strategy](strategy.md) for the full `emit*` list and the pre-trade gate ordering.
