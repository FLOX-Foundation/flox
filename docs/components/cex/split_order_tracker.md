# SplitOrderTracker

Track parent-child order relationships for split order execution.

## Header

```cpp
#include "flox/execution/split_order_tracker.h"
```

## Synopsis

```cpp
class SplitOrderTracker
{
public:
  static constexpr size_t kMaxChildrenPerSplit = 8;

  struct SplitState {
    OrderId parentId{};
    std::array<OrderId, kMaxChildrenPerSplit> childIds{};
    uint8_t childCount{0};
    uint8_t completedCount{0};
    uint8_t failedCount{0};
    int64_t totalQtyRaw{0};
    int64_t filledQtyRaw{0};
    int64_t createdAtNs{0};

    bool allDone() const noexcept;
    bool allSuccess() const noexcept;
    double fillRatio() const noexcept;
  };

  // Register a split order
  bool registerSplit(OrderId parent, std::span<const OrderId> children,
                     int64_t totalQtyRaw, int64_t nowNs) noexcept;

  // Update on child events
  void onChildFill(OrderId childId, int64_t fillQtyRaw) noexcept;
  void onChildComplete(OrderId childId, bool success) noexcept;

  // Query
  const SplitState* getState(OrderId parentId) const noexcept;
  bool isComplete(OrderId parentId) const noexcept;
  bool isSuccessful(OrderId parentId) const noexcept;

  // Management
  void cleanup(int64_t nowNs, int64_t timeoutNs) noexcept;
  size_t size() const noexcept;
};
```

## Use Case

When executing a large order across multiple exchanges, split it into child orders and track their aggregate status:

```
Parent Order (1000 qty)
    ├── Child 1 (Binance): 400 qty
    ├── Child 2 (Bybit): 350 qty
    └── Child 3 (Kraken): 250 qty
```

## Usage

### Register Split Order

```cpp
SplitOrderTracker tracker;

// Parent order 1000 split into 3 children
std::array<OrderId, 3> children = {1001, 1002, 1003};
Quantity totalQty = Quantity::fromDouble(10.0);  // 10 BTC
int64_t nowNs = /* current time */;

bool ok = tracker.registerSplit(1000, children, totalQty.raw(), nowNs);
if (!ok) {
  // Failed - parent already exists or too many children
}
```

### Track Fills

```cpp
// Child 1001 filled 4 BTC
tracker.onChildFill(1001, Quantity::fromDouble(4.0).raw());

// Child 1002 filled 3.5 BTC
tracker.onChildFill(1002, Quantity::fromDouble(3.5).raw());

// Check fill ratio
auto* state = tracker.getState(1000);
std::cout << "Fill ratio: " << (state->fillRatio() * 100) << "%\n";
// Output: Fill ratio: 75%
```

### Track Completion

```cpp
// Mark children as complete
tracker.onChildComplete(1001, true);   // Success
tracker.onChildComplete(1002, true);   // Success
tracker.onChildComplete(1003, false);  // Failed (e.g., rejected)

// Check overall status
if (tracker.isComplete(1000)) {
  if (tracker.isSuccessful(1000)) {
    std::cout << "All children succeeded\n";
  } else {
    std::cout << "Some children failed\n";
  }
}
```

### Cleanup Old Entries

```cpp
// Remove splits older than 1 hour
constexpr int64_t oneHourNs = 3600LL * 1'000'000'000LL;
tracker.cleanup(nowNs, oneHourNs);
```

## SplitState Fields

| Field | Description |
|-------|-------------|
| `parentId` | Parent order ID |
| `childIds` | Array of child order IDs |
| `childCount` | Number of children |
| `completedCount` | Children completed (success or fail) |
| `failedCount` | Children that failed |
| `totalQtyRaw` | Total quantity to fill |
| `filledQtyRaw` | Quantity filled so far |
| `createdAtNs` | Creation timestamp for timeout cleanup |

## Helper Methods

### allDone()
```cpp
bool allDone() const noexcept {
  return completedCount + failedCount >= childCount;
}
```

### allSuccess()
```cpp
bool allSuccess() const noexcept {
  return completedCount >= childCount && failedCount == 0;
}
```

### fillRatio()
```cpp
double fillRatio() const noexcept {
  return totalQtyRaw > 0 ? static_cast<double>(filledQtyRaw) / totalQtyRaw : 0.0;
}
```

## Limits

- Maximum children per split: 8 (`kMaxChildrenPerSplit`)
- Attempting to register more children returns `false`

## Performance

| Operation | Complexity |
|-----------|------------|
| registerSplit() | O(children) |
| onChildFill() | O(1) amortized |
| onChildComplete() | O(1) amortized |
| getState() | O(1) amortized |
| cleanup() | O(n) where n = active splits |

Uses hash maps internally for O(1) lookups.

## See Also

- [OrderRouter](order_router.md) - Route split orders to exchanges
- [AggregatedPositionTracker](aggregated_position_tracker.md) - Track aggregate position from fills
