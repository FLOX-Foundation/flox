# SymbolStateMap

`SymbolStateMap` is a high-performance O(1) container for per-symbol state, optimized for trading systems.

```cpp
template <typename State, size_t MaxSymbols = 256>
class SymbolStateMap;
```

## Purpose

- Provide O(1) access to per-symbol state
- Optimize for the common case (symbol IDs < 256)
- Handle overflow gracefully for large symbol counts
- Cache-line aligned for optimal memory access

## Design

The container uses a two-tier approach:

1. **Flat array** (symbols 0-255): Direct O(1) access, cache-line aligned
2. **Overflow vector** (symbols >= 256): Linear search, rare case

```cpp
alignas(64) std::array<State, MaxSymbols> _flat{};
std::array<bool, MaxSymbols> _initialized{};
std::vector<std::pair<SymbolId, State>> _overflow;
```

## API

### Access

```cpp
// Get or create state for symbol (O(1) for sym < 256)
State& operator[](SymbolId symbol) noexcept;
const State& operator[](SymbolId symbol) const noexcept;

// Get pointer if exists, nullptr otherwise
State* tryGet(SymbolId symbol) noexcept;
const State* tryGet(SymbolId symbol) const noexcept;

// Check if symbol has state
bool contains(SymbolId symbol) const noexcept;
```

### Iteration

```cpp
// Iterate over all initialized symbols
template <typename Func>
void forEach(Func&& fn);

// Func signature: void(SymbolId, State&)
```

### Utilities

```cpp
void clear() noexcept;          // Clear all state
size_t size() const noexcept;   // Count of initialized symbols
```

## Example

```cpp
// Per-symbol position tracking
struct PositionState
{
  double quantity{0.0};
  double avgPrice{0.0};
  double realizedPnl{0.0};
};

SymbolStateMap<PositionState> positions;

// Update position
positions[btcSymbol].quantity += 10.0;
positions[btcSymbol].avgPrice = 50000.0;

// Check if tracked
if (positions.contains(ethSymbol))
{
  auto& state = positions[ethSymbol];
  // ...
}

// Iterate all positions
positions.forEach([](SymbolId sym, PositionState& pos) {
  std::cout << "Symbol " << sym << ": " << pos.quantity << "\n";
});

// Accumulate total PnL
double totalPnl = 0.0;
positions.forEach([&totalPnl](SymbolId, const PositionState& pos) {
  totalPnl += pos.realizedPnl;
});
```

## Performance

| Operation | Complexity (sym < 256) | Complexity (sym >= 256) |
|-----------|------------------------|-------------------------|
| `operator[]` | O(1) | O(n) overflow search |
| `contains` | O(1) | O(n) overflow search |
| `tryGet` | O(1) | O(n) overflow search |
| `forEach` | O(MaxSymbols) | O(MaxSymbols + n) |

For most trading systems with < 256 symbols, all operations are O(1).

## Memory

- Flat array: `MaxSymbols * sizeof(State)` bytes (64-byte aligned)
- Initialized flags: `MaxSymbols` bytes
- Overflow: dynamic allocation only when needed

Default with `SymbolContext`: ~1MB for 256 symbols (4KB per symbol due to order book).

## Template Parameters

```cpp
template <typename State, size_t MaxSymbols = 256>
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `State` | - | Per-symbol state type |
| `MaxSymbols` | 256 | Flat array size, symbols >= this go to overflow |

## See Also

- [SymbolContext](symbol_context.md) - Per-symbol state struct
- [Strategy](strategy.md) - Uses SymbolStateMap internally
