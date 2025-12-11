# CpuAffinity

The `CpuAffinity` class provides a unified interface for managing thread affinity, real-time scheduling, NUMA policies, and performance-oriented CPU assignment. It serves as a high-level facade over several specialized components.

## Purpose

- Pin threads and components to specific CPU cores
- Optimize for latency via isolated cores and NUMA locality
- Abstract platform details through injected interfaces

## Composition

`CpuAffinity` internally coordinates the following subsystems:

| Component | Responsibility |
|----------|----------------|
| `ISystemInterface` | Platform-specific system operations |
| `CpuTopology` | Logical/physical CPU and NUMA layout |
| `ThreadAffinity` | Pinning and scheduling logic |
| `CoreAssignmentManager` | Allocation of roles to CPU cores |

## Key Features

### Thread Affinity

- `pinToCore(int coreId)`
- `pinToCore(std::thread&, int coreId)`
- `getCurrentAffinity()`

### Scheduling

- `setRealTimePriority(int priority = 80)`
- `setRealTimePriority(std::thread&, int priority = 80)`

### Core Management

- `getNumCores()`
- `getIsolatedCores()`
- `disableCpuFrequencyScaling()`
- `enableCpuFrequencyScaling()`

### Assignment Strategies

- `getRecommendedCoreAssignment(...)`
- `getBasicCoreAssignment(...)`
- `getNumaAwareCoreAssignment(...)`
- `pinCriticalComponent(...)`
- `verifyCriticalCoreIsolation(...)`

### NUMA Awareness

- `getNumaTopology()`
- `getNumaNodeForCore(int coreId)`
- `pinToNumaNode(int nodeId)`
- `setMemoryPolicy(int nodeId)`

## Lifecycle

`CpuAffinity` can be created via its constructor or with the helper:

```cpp
auto cpuAffinity = createCpuAffinity();
````

All subsystems will be lazily instantiated with default implementations unless provided explicitly for testing.

## RAII: `NumaAffinityGuard`

Wraps the calling thread with a temporary NUMA and affinity setting:

```cpp
{
  NumaAffinityGuard guard(cpuAffinity, coreId, numaNodeId);
  // thread is pinned and memory policy applied
}
// automatically restored on destruction
```

## Example

```cpp
auto affinity = createCpuAffinity();

if (affinity->checkIsolatedCoreRequirements()) {
  auto layout = affinity->getRecommendedCoreAssignment();
  affinity->pinCriticalComponent("execution", layout);
  affinity->setRealTimePriority();
}
```

## Integration

Used by performance-critical modules such as:

* Strategy engine threads
* Market data collectors
* Execution pipelines

It ensures low-latency execution and determinism by aligning component-to-core layout with hardware topology.
