# ThreadAffinity

The `ThreadAffinity` class provides a unified interface for managing thread affinity, real-time priority, and CPU frequency policies in latency-critical systems. It is used to enforce deterministic scheduling behavior and optimal placement on isolated cores.

## Responsibilities

| Capability | Description |
|------------|-------------|
| Pinning    | Assign threads (or current thread) to specific core(s) |
| Priority   | Set real-time thread priorities (SCHED_FIFO) |
| NUMA       | Set memory allocation policy for NUMA locality |
| Governors  | Switch CPU frequency governors for performance consistency |
| Validation | Verify whether selected cores are isolated |

## Public Interface

### Pinning

```cpp
bool pinCurrentThreadToCore(int coreId);
bool pinCurrentThreadToCores(const std::vector<int>& coreIds);
bool pinThreadToCore(std::thread&, int coreId);
bool pinThreadToCores(std::thread&, const std::vector<int>& coreIds);
````

### Priority

```cpp
bool setCurrentThreadPriority(int priority = 80);
bool setThreadPriority(std::thread&, int priority = 80);
```

### Affinity & NUMA

```cpp
std::vector<int> getCurrentThreadAffinity();
bool setCurrentThreadNumaPolicy(int nodeId);
```

### CPU Frequency Policy

```cpp
bool disableCpuFrequencyScaling();  // "performance"
bool enableCpuFrequencyScaling();   // "powersave"
```

### Validation

```cpp
bool verifyCriticalCoreIsolation(const std::vector<int>& cores);
```

## Usage Example

```cpp
ThreadAffinity affinity(createSystemInterface());

affinity.pinCurrentThreadToCore(2);
affinity.setCurrentThreadPriority(90);
affinity.disableCpuFrequencyScaling();
```

## ThreadAffinityGuard

RAII wrapper that ensures temporary pinning to a specific core or cores.

```cpp
{
  ThreadAffinityGuard guard(3);  // Pins thread to core 3 temporarily
  // Do latency-sensitive work here
}  // Original affinity is restored automatically
```

## Design Notes

* Uses `ISystemInterface` for portability and mocking
* Avoids OS-specific code in high-level components
* Defaults to `performance` governor to eliminate CPU frequency variance
* Isolated cores are detected via `/proc/cmdline` (`isolcpus=...`)

## Integration

Used directly or via:

* `CpuAffinity` as part of performance toolkit
* `NumaAffinityGuard` for NUMA-aware scoped locality
* Benchmark tools and real-time strategy components in FLOX
