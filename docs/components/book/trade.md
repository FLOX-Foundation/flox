# Trade

`Trade` represents a single executed transaction between a buyer and seller on a specific symbol, including price, quantity, and taker side.

```cpp
struct Trade {
  SymbolId symbol{};
  Price price{};
  Quantity quantity{};
  bool isBuy{false};
  std::chrono::steady_clock::time_point timestamp{};
};
```

## Purpose

* Convey executed market activity in a normalized format for downstream processing.

## Responsibilities

| Field     | Description                                                  |
| --------- | ------------------------------------------------------------ |
| symbol    | Identifies the traded instrument.                            |
| price     | Execution price of the trade.                                |
| quantity  | Size of the trade (in base units).                           |
| isBuy     | True if taker was buyer; false if taker was seller.          |
| timestamp | Wall-clock or event time of execution, using `steady_clock`. |

## Notes

* Used in `TradeEvent`, which is dispatched through `TradeBus`.
* Serves as input for aggregation, PnL tracking, and order flow analysis.
* `isBuy` expresses taker aggressiveness — not resting order direction.
