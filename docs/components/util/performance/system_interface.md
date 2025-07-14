# SystemInterface

The `SystemInterface` module provides an abstraction over platform-specific system operations related to CPU affinity, thread priority, NUMA configuration, and file I/O. It serves as the foundation for CPU-level performance tuning and is primarily used by `CpuAffinity` and related components.

## Purpose

- Isolate system calls from platform-independent logic
- Enable mocking and testing via `NullSystemInterface`
- Provide unified access to thread and CPU-level control

## Interface: `ISystemInterface`

### Affinity

| Method | Description |
|--------|-------------|
| `setThreadAffinity(pthread_t thread, const std::vector<int>& cores)` | Pins a specific thread to the specified cores |
| `setCurrentThreadAffinity(const std::vector<int>& cores)` | Pins the current thread to the specified cores |
| `getCurrentThreadAffinity()` | Returns the current thread’s CPU affinity |

### Thread Priority

| Method | Description |
|--------|-------------|
| `setThreadPriority(pthread_t thread, int priority)` | Sets real-time priority (SCHED_FIFO) for a specific thread |
| `setCurrentThreadPriority(int priority)` | Sets real-time priority for the current thread |

### Topology & NUMA

| Method | Description |
|--------|-------------|
| `getNumCores()` | Returns the total number of logical CPU cores |
| `getIsolatedCores()` | Returns list of isolated cores from `isolcpus=` kernel argument |
| `getNumaNodes()` | Returns list of NUMA nodes and associated core IDs |
| `getNumaNodeForCore(int coreId)` | Returns NUMA node ID for the given core |
| `setMemoryPolicy(int nodeId)` | Sets preferred memory allocation policy for the current thread |
| `isNumaAvailable()` | Returns true if NUMA is supported and available |

### File I/O

| Method | Description |
|--------|-------------|
| `readFile(const std::string& path)` | Reads the content of a file |
| `writeFile(const std::string& path, const std::string& content)` | Writes content to a file |

## Implementations

### `LinuxSystemInterface`

Linux-specific implementation based on:

- `sched_setaffinity`, `pthread_setschedparam`, `sched_setscheduler`
- NUMA support via `<numa.h>` and `/sys/devices/system/node/`
- Parses `/proc/cmdline` for `isolcpus=`
- Reads and writes files through `std::ifstream` / `std::ofstream`

### `NullSystemInterface`

Dummy fallback used on unsupported platforms or in tests:

- All methods return `false` or empty/default values
- Safe no-op implementation

## NUMA Support

NUMA is considered available if either:

1. The system links with `libnuma` and headers are present (`FLOX_NUMA_LIBRARY_LINKED`)
2. `/sys/devices/system/node/` contains valid `node*` directories and `cpulist` files

## Factory Function

```cpp
std::unique_ptr<ISystemInterface> createSystemInterface();
````

Creates a platform-appropriate implementation:

* `LinuxSystemInterface` on Linux
* `NullSystemInterface` otherwise

## Example

```cpp
auto system = createSystemInterface();

if (system->isNumaAvailable()) {
  auto nodes = system->getNumaNodes();
  if (!nodes.empty()) {
    system->setCurrentThreadAffinity({nodes[0].second.front()});
    system->setMemoryPolicy(nodes[0].first);
  }
}
```

## Integration

This interface is consumed by:

* `ThreadAffinity`
* `CpuTopology`
* `CoreAssignmentManager`
* `CpuAffinity`

It decouples platform-specific logic and enables system-aware optimization in a portable way.
