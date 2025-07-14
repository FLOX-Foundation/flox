# CoreAssignmentManager

`CoreAssignmentManager` is responsible for assigning CPU cores to critical components in the Flox Engine to optimize performance, minimize contention, and leverage CPU topology such as isolated cores and NUMA awareness.

## Overview

Flox supports assigning cores to the following critical components:

- `marketData` — Market data collection and processing
- `execution` — Order routing and order execution
- `strategy` — Strategy computation and signal generation
- `risk` — Risk validation and rejection filtering

These components can be pinned to isolated or shared cores based on a configurable policy.

## Configuration: CriticalComponentConfig

This config defines how isolated and shared cores are assigned:

```cpp
struct CriticalComponentConfig
{
  bool preferIsolatedCores;       // Prefer isolated cores if available
  bool exclusiveIsolatedCores;    // Use isolated cores exclusively (no sharing)
  bool allowSharedCriticalCores;  // Allow components to share cores
  int minIsolatedForCritical;     // Minimum isolated cores required
  std::map<std::string, int> componentPriority; // Lower value = higher priority
};
```

Default priority:

| Component    | Priority |
| ------------ | -------- |
| `marketData` | 0        |
| `execution`  | 1        |
| `strategy`   | 2        |
| `risk`       | 3        |

## CoreAssignment Result

After assignment, `CoreAssignment` structure contains:

* `.marketDataCores`, `.executionCores`, `.strategyCores`, `.riskCores`
* `.generalCores` — fallback cores for non-critical components
* `.hasIsolatedCores` — whether isolated cores were used
* `.criticalCores` — union of all critical components' cores

## Assignment Strategies

| Method                                  | Description                                   |
| --------------------------------------- | --------------------------------------------- |
| `getRecommendedCoreAssignment()`        | Chooses optimal strategy based on config      |
| `getNumaAwareCoreAssignment()`          | Prefer NUMA-balanced layout if isolated cores |
| `getBasicCoreAssignment(num, isolated)` | Fallback round-robin assignment               |

## Affinity Pinning

You can pin the current thread to the assigned cores for a given component:

```cpp
bool success = pinCriticalComponent("marketData", assignment);
```

To pin all components at once:

```cpp
setupAndPinCriticalComponents(config);
```

## Validation and Debugging

| Method                            | Purpose                                       |
| --------------------------------- | --------------------------------------------- |
| `verifyCriticalCoreIsolation()`   | Ensures all critical cores are truly isolated |
| `checkIsolatedCoreRequirements()` | Checks availability of minimum isolated cores |
| `demonstrateIsolatedCoreUsage()`  | Logs current assignment and core layout       |

Example log output:

```
=== CPU Affinity and Isolated Core Usage Demonstration ===
Total CPU cores: 12
Isolated cores: 2 3 4
Recommended core assignment:
Market Data cores: 2
Execution cores: 3
Strategy cores: 4
Risk cores: 5
General cores: 6 7 8 9 10 11
```

## Internals

Under the hood, `CoreAssignmentManager` uses:

* `CpuTopology` — queried for isolated and non-isolated cores
* `ThreadAffinity` — pins threads to specified cores using `sched_setaffinity()`
* `NumaTopology` — optionally used to balance across NUMA nodes

```cpp
ThreadAffinity affinity(createSystemInterface());
affinity.pinCurrentThreadToCores({3});
```

## Notes

* Default policy uses **one core per critical component**.
* Isolated cores are preferred and assigned to higher priority components.
* General tasks are assigned remaining cores.
* This module is **optional**: core pinning will only occur if explicitly enabled.
