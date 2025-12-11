# IOrderBook

`IOrderBook` defines the abstract interface for order book implementations that consume `BookUpdateEvent`s and provide access to market depth and price levels.

```cpp
class IOrderBook {
public:
  virtual ~IOrderBook() = default;

  virtual void applyBookUpdate(const BookUpdateEvent& update) = 0;
  virtual std::optional<Price> bestBid() const = 0;
  virtual std::optional<Price> bestAsk() const = 0;

  virtual Quantity bidAtPrice(Price price) const = 0;
  virtual Quantity askAtPrice(Price price) const = 0;
};
```

## Purpose

* Define a common contract for order book consumers, simulators, and strategy components.

## Responsibilities

| Aspect      | Details                                                          |
| ----------- | ---------------------------------------------------------------- |
| Update      | `applyBookUpdate()` ingests raw changes from `BookUpdateEvent`.  |
| Top of book | `bestBid()` / `bestAsk()` expose inside market prices.           |
| Depth query | `bidAtPrice()` / `askAtPrice()` return size at arbitrary levels. |

## Notes

* Stateless interface — actual book implementation (e.g. `NLevelOrderBook`) manages memory and performance.
* Compatible with pooled update dispatch via `BookUpdateBus`.
* Returns `std::optional` for top-of-book queries to reflect potential emptiness.
