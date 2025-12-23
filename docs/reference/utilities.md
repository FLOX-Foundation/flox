# Utilities Reference

Pools, queues, decimal types, and performance utilities.

## Decimal Types

**Header:** `flox/util/base/decimal.h`

Fixed-point arithmetic for prices and quantities.

```cpp
template <typename Tag, int Scale_, int64_t TickSize_ = 1>
class Decimal
{
public:
  static constexpr int Scale = Scale_;
  static constexpr int64_t TickSize = TickSize_;

  constexpr Decimal();
  explicit constexpr Decimal(int64_t raw);

  // Construction
  static constexpr Decimal fromDouble(double val);
  static constexpr Decimal fromRaw(int64_t raw);

  // Conversion
  constexpr double toDouble() const;
  constexpr int64_t raw() const;
  std::string toString() const;

  // Tick operations
  constexpr Decimal roundToTick() const;

  // Arithmetic
  constexpr Decimal operator+(Decimal d) const;
  constexpr Decimal operator-(Decimal d) const;
  constexpr Decimal operator*(Decimal d) const;
  constexpr Decimal operator/(Decimal d) const;
  constexpr Decimal operator*(int64_t x) const;
  constexpr Decimal operator/(int64_t x) const;

  // Compound assignment
  constexpr Decimal& operator+=(const Decimal& other);
  constexpr Decimal& operator-=(const Decimal& other);
  constexpr Decimal& operator*=(const Decimal& other);
  constexpr Decimal& operator/=(const Decimal& other);

  // Comparison
  constexpr auto operator<=>(const Decimal&) const = default;
  constexpr bool operator==(const Decimal&) const = default;

  constexpr bool isZero() const;
};
```

### Predefined Types

```cpp
// All use Scale = 100,000,000 (8 decimal places, matches Binance precision)
using Price = Decimal<PriceTag, 100'000'000, 1>;
using Quantity = Decimal<QuantityTag, 100'000'000, 1>;
using Volume = Decimal<VolumeTag, 100'000'000, 1>;
```

### Examples

```cpp
Price p1 = Price::fromDouble(100.50);
Price p2 = Price::fromDouble(0.01);

Price sum = p1 + p2;           // 100.51
double d = sum.toDouble();     // 100.51
int64_t raw = sum.raw();       // 10051000000 (100.51 * 10^8)

Quantity q = Quantity::fromDouble(10.5);
Volume v = q * Price::fromDouble(100.0);  // Type-safe: Quantity * Price = Volume
```

### Cross-Type Arithmetic

The following operators handle 128-bit intermediate calculations to prevent overflow:

```cpp
Volume notional = qty * price;      // Quantity * Price = Volume
Price avgPrice = volume / qty;      // Volume / Quantity = Price
Quantity baseQty = volume / price;  // Volume / Price = Quantity
```

---

## SPSCQueue

**Header:** `flox/util/concurrency/spsc_queue.h`

Lock-free single-producer single-consumer queue.

```cpp
template <typename T, size_t Capacity>
class SPSCQueue
{
public:
  SPSCQueue();
  ~SPSCQueue();

  // Push (returns false if full)
  bool push(const T& item) noexcept;
  bool emplace(T&& item) noexcept;

  template <typename... Args>
  bool try_emplace(Args&&... args);

  // Pop (returns false if empty)
  bool pop(T& out) noexcept;
  T* try_pop();
  std::optional<std::reference_wrapper<T>> try_pop_ref();

  // State
  void clear() noexcept;
  bool empty() const noexcept;
  bool full() const noexcept;
  size_t size() const noexcept;
};
```

### Requirements

- `Capacity` must be > 1 and power of 2
- `T` must be nothrow destructible

### Example

```cpp
SPSCQueue<int, 1024> queue;

// Producer thread
queue.push(42);
queue.emplace(43);

// Consumer thread
int value;
if (queue.pop(value)) {
  // Process value
}
```

---

## Object Pool

**Header:** `flox/util/memory/pool.h`

Pre-allocated object pool for zero-allocation event handling.

### Pool

```cpp
template <typename T, size_t Capacity>
class Pool
{
public:
  using ObjectType = T;

  Pool();
  ~Pool();

  std::optional<Handle<T>> acquire();  // Get object from pool
  void release(T* obj);                // Return to pool (automatic via Handle)

  size_t inUse() const;
};
```

### Handle

Reference-counted smart pointer for pooled objects.

```cpp
template <typename T>
class Handle
{
public:
  explicit Handle(T* ptr) noexcept;
  Handle(const Handle& other) noexcept;     // Copy: increments refcount
  Handle(Handle&& other) noexcept;          // Move: transfers ownership
  ~Handle();                                 // Returns to pool when refcount=0

  T* get() const noexcept;
  T* operator->() const noexcept;
  T& operator*() const noexcept;

  template <typename U>
  Handle<U> upcast() const;
};
```

### PoolableBase

Base class for pooled objects.

```cpp
template <typename Derived>
struct PoolableBase : public RefCountable
{
  void setPool(void* pool);
  void releaseToPool();
  void clear();  // Override to reset object state
};
```

### Example

```cpp
struct MyEvent : public pool::PoolableBase<MyEvent>
{
  int data;
  std::pmr::vector<int> items;

  MyEvent(std::pmr::memory_resource* res) : items(res) {}

  void clear() {
    data = 0;
    items.clear();
  }
};

// Create pool
pool::Pool<MyEvent, 64> eventPool;

// Acquire
if (auto handle = eventPool.acquire()) {
  (*handle)->data = 42;
  (*handle)->items.push_back(1);

  // Pass to bus
  bus.publish(std::move(handle));
}
// Handle automatically returns to pool when refcount reaches 0
```

---

## RefCountable

**Header:** `flox/util/memory/ref_countable.h`

Thread-safe reference counting.

```cpp
class RefCountable
{
public:
  RefCountable() = default;
  virtual ~RefCountable() = default;

  void retain();                    // Increment refcount
  bool release();                   // Decrement; returns true if refcount hit 0
  void resetRefCount();             // Reset to 1
  uint32_t refCount() const;
};
```

---

## BusyBackoff

**Header:** `flox/util/performance/busy_backoff.h`

Adaptive backoff strategy for spin loops.

```cpp
class BusyBackoff
{
public:
  void pause();   // Spin, yield, or sleep based on iteration count
  void reset();   // Reset iteration counter
};
```

### Strategy

1. First ~100 iterations: CPU pause instruction
2. Next ~100 iterations: `std::this_thread::yield()`
3. Beyond: Short sleep (microseconds)

---

## CPU Affinity

**Header:** `flox/util/performance/cpu_affinity.h`

Available when compiled with `FLOX_ENABLE_CPU_AFFINITY=ON`.

```cpp
class CpuAffinity
{
public:
  bool pinToCore(int coreId);
  bool setRealTimePriority(int priority);
  bool disableCpuFrequencyScaling();

  CoreAssignment getRecommendedCoreAssignment(const CriticalComponentConfig& config);
  CoreAssignment getNumaAwareCoreAssignment(const CriticalComponentConfig& config);

  bool verifyCriticalCoreIsolation(const CoreAssignment& assignment);
};

struct CriticalComponentConfig
{
  bool preferIsolatedCores = true;
  bool exclusiveIsolatedCores = true;
  bool allowSharedCriticalCores = false;
};

struct CoreAssignment
{
  std::vector<int> marketDataCores;
  std::vector<int> executionCores;
  std::vector<int> strategyCores;
  std::vector<int> riskCores;
  std::vector<int> generalCores;
  std::vector<int> allIsolatedCores;
  bool hasIsolatedCores{false};
};

// Factory function
std::unique_ptr<CpuAffinity> createCpuAffinity();
```

---

## Logging

**Header:** `flox/log/log.h`

```cpp
// Macros
FLOX_LOG(message);      // Log message
FLOX_LOG_OFF();         // Disable logging
FLOX_LOG_ON();          // Enable logging
```

### Example

```cpp
FLOX_LOG("Trade received: " << ev.trade.price.toDouble());
```

---

## Profiling

**Header:** `flox/util/performance/profile.h`

Tracy profiler integration (when `FLOX_ENABLE_TRACY=ON`).

```cpp
// Scope profiling
FLOX_PROFILE_SCOPE("MyFunction");

// Without Tracy, these are no-ops
```

---

## Time Utilities

**Header:** `flox/util/base/time.h`

```cpp
using UnixNanos = int64_t;   // Nanoseconds since Unix epoch
using MonoNanos = int64_t;   // Nanoseconds from monotonic clock
using TimePoint = std::chrono::system_clock::time_point;

// Functions
UnixNanos nowNsWallclock();
MonoNanos nowNsMonotonic();
void init_timebase_mapping();  // Call once at startup
```

---

## See Also

- [Memory Model](../explanation/memory-model.md) — Zero-allocation design
- [The Disruptor Pattern](../explanation/disruptor.md) — Lock-free event delivery
