# FLOX_FORCE_INLINE

Cross-platform macro for forcing function inlining on performance-critical code paths.

## Header

```cpp
#include "flox/util/performance/force_inline.h"
```

## Definition

```cpp
#ifdef _MSC_VER
#define FLOX_FORCE_INLINE __forceinline
#else
#define FLOX_FORCE_INLINE __attribute__((always_inline)) inline
#endif
```

## Usage

```cpp
class MyClass {
public:
  FLOX_FORCE_INLINE void hotPathMethod() noexcept {
    // Performance-critical code
  }
};
```

## When to Use

Use `FLOX_FORCE_INLINE` on:
- Hot path functions called millions of times per second
- Small functions where call overhead exceeds function body
- Template methods that must inline for performance
- Functions in tight loops

## When NOT to Use

Avoid on:
- Large functions (increases code size, cache pressure)
- Cold paths (rarely executed code)
- Virtual functions (cannot be inlined anyway)
- Recursive functions

## Platform Behavior

| Compiler | Implementation | Notes |
|----------|----------------|-------|
| MSVC | `__forceinline` | Strong hint, may still not inline in debug builds |
| GCC | `__attribute__((always_inline)) inline` | Forces inline unless impossible |
| Clang | `__attribute__((always_inline)) inline` | Same as GCC |

## Example: MultiTimeframeAggregator

```cpp
template <size_t MaxTimeframes>
class MultiTimeframeAggregator {
private:
  FLOX_FORCE_INLINE void processSlot(size_t slotIdx, const TradeEvent& trade) {
    // Called for every trade, for every timeframe
    // ~10ns overhead if not inlined
  }

  template <typename Policy>
  FLOX_FORCE_INLINE void processPolicy(Policy& policy, ...) {
    // Policy dispatch - must inline for zero-cost abstraction
  }
};
```

## Verification

Use `objdump -d` or compiler explorer to verify inlining:

```bash
# Check if function was inlined (should NOT appear as separate symbol)
nm ./build/benchmarks/bar_aggregator_benchmark | grep processSlot
```

## See Also

- [CPU Affinity](cpu_affinity.md) - Pin threads to cores
- [Thread Affinity](thread_affinity.md) - Thread-to-core binding
