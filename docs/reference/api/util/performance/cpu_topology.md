# CpuTopology

The `CpuTopology` class provides system-level introspection for CPU layout and NUMA topology. It is used to understand the physical and logical arrangement of cores and to optimize thread and memory placement.

## Purpose

- Determine number of physical CPU cores
- Identify isolated and non-isolated cores
- Access detailed NUMA topology (if available)
- Map CPU cores to their NUMA nodes

## Key Structures

### `NumaNode`
```cpp
struct NumaNode {
  int nodeId;
  std::vector<int> cpuCores;
  size_t totalMemoryMB;
  size_t freeMemoryMB;
};
````

### `NumaTopology`

```cpp
struct NumaTopology {
  std::vector<NumaNode> nodes;
  int numNodes;
  bool numaAvailable;
};
```

## Responsibilities

| Method                        | Description                                            |
| ----------------------------- | ------------------------------------------------------ |
| `getNumCores()`               | Returns total number of logical cores                  |
| `getAllCores()`               | Returns list of core indices from 0 to N-1             |
| `getIsolatedCores()`          | Returns list of isolated CPUs (from cmdline)           |
| `getNonIsolatedCores()`       | All cores minus isolated ones                          |
| `isNumaAvailable()`           | Whether NUMA support is available on the system        |
| `getNumaTopology()`           | Returns detailed info about all NUMA nodes             |
| `getNumaNodeForCore(coreId)`  | Returns the NUMA node to which a specific core belongs |
| `getCoresForNumaNode(nodeId)` | Lists cores that belong to a given NUMA node           |

## Lazy Caching

`CpuTopology` caches:

* Core count
* Isolated cores
* NUMA layout

This minimizes redundant syscalls and file reads.

## Usage Example

```cpp
CpuTopology topology(createSystemInterface());

if (topology.isNumaAvailable()) {
  auto nodes = topology.getNumaTopology().nodes;
  for (const auto& node : nodes) {
    std::cout << "NUMA Node " << node.nodeId << ": ";
    for (int core : node.cpuCores)
      std::cout << core << " ";
    std::cout << "\n";
  }
}
```

## Integration

`CpuTopology` is used by:

* `CpuAffinity` for pinning and scheduling
* `CoreAssignmentManager` for optimized component placement
* `NumaAffinityGuard` for temporary core+memory locality enforcement

It provides the necessary insights into hardware layout to guide all thread and memory affinity operations.
