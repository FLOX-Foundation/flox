# Configure CPU Affinity

Pin threads to isolated CPU cores for lower, more predictable latency.

## When to Use

CPU affinity is useful when:
- You have dedicated hardware for trading
- You've isolated CPU cores from the OS scheduler
- You need sub-microsecond latency consistency

**Warning:** CPU affinity can *decrease* performance on shared or busy systems. Only use it when you control the entire system workload.

## 1. Enable at Build Time

```bash
cmake .. -DFLOX_ENABLE_CPU_AFFINITY=ON
```

This requires the NUMA library on Linux:
```bash
sudo apt install libnuma-dev  # Debian/Ubuntu
```

## 2. Isolate CPU Cores (System Setup)

Edit your kernel command line (e.g., `/etc/default/grub`):

```
GRUB_CMDLINE_LINUX="isolcpus=2,3,4,5 nohz_full=2,3,4,5 rcu_nocbs=2,3,4,5"
```

Then:
```bash
sudo update-grub
sudo reboot
```

Verify isolation:
```bash
cat /sys/devices/system/cpu/isolated
# Should show: 2-5
```

## 3. Configure EventBus Affinity

### Automatic Configuration

```cpp
#include "flox/book/bus/trade_bus.h"

TradeBus tradeBus;

// Auto-configure for market data workload
tradeBus.setupOptimalConfiguration(
    TradeBus::ComponentType::MARKET_DATA,
    /*enablePerformanceOptimizations=*/true
);
```

Component types and their priority:
| Type | Priority | Use For |
|------|----------|---------|
| `MARKET_DATA` | 90 | Market data buses |
| `EXECUTION` | 85 | Order execution bus |
| `STRATEGY` | 80 | Strategy processing |
| `RISK` | 75 | Risk management |
| `GENERAL` | 70 | Everything else |

### Manual Configuration

```cpp
#include "flox/util/performance/cpu_affinity.h"

using namespace flox::performance;

// Create affinity manager
auto cpuAffinity = createCpuAffinity();

// Get recommended core assignment
CriticalComponentConfig config;
config.preferIsolatedCores = true;
config.exclusiveIsolatedCores = true;

auto assignment = cpuAffinity->getNumaAwareCoreAssignment(config);

// Apply to bus
tradeBus.setCoreAssignment(assignment);

// Verify configuration
if (tradeBus.verifyIsolatedCoreConfiguration()) {
    std::cout << "CPU affinity configured correctly" << std::endl;
}
```

### Per-Bus Configuration

```cpp
// Different buses can use different core types
TradeBus::AffinityConfig tradeCfg;
tradeCfg.componentType = TradeBus::ComponentType::MARKET_DATA;
tradeCfg.enableRealTimePriority = true;
tradeCfg.realTimePriority = 90;
tradeCfg.enableNumaAwareness = true;
tradeCfg.preferIsolatedCores = true;

tradeBus.setAffinityConfig(tradeCfg);

OrderExecutionBus::AffinityConfig execCfg;
execCfg.componentType = OrderExecutionBus::ComponentType::EXECUTION;
execCfg.enableRealTimePriority = true;
execCfg.realTimePriority = 85;

execBus.setAffinityConfig(execCfg);
```

## 4. Core Assignment Structure

```cpp
struct CoreAssignment
{
  std::vector<int> marketDataCores;  // For market data processing
  std::vector<int> executionCores;   // For order execution
  std::vector<int> strategyCores;    // For strategy threads
  std::vector<int> riskCores;        // For risk management
  std::vector<int> generalCores;     // For non-critical threads
  std::vector<int> allIsolatedCores; // All isolated cores
  bool hasIsolatedCores{false};      // True if system has isolated cores
};
```

## 5. NUMA Awareness

For multi-socket systems, use NUMA-aware assignment:

```cpp
auto assignment = cpuAffinity->getNumaAwareCoreAssignment(config);
```

This ensures:
- Threads run on cores near their memory
- Cross-socket memory access is minimized
- Cache coherency traffic is reduced

## 6. Disable Frequency Scaling

For consistent performance, disable CPU frequency scaling:

```cpp
cpuAffinity->disableCpuFrequencyScaling();
```

Or via system settings:
```bash
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

## 7. Verify Configuration

```cpp
// Check if isolated cores are properly configured
if (tradeBus.verifyIsolatedCoreConfiguration()) {
    std::cout << "Verified: running on isolated cores" << std::endl;
} else {
    std::cerr << "Warning: not running on isolated cores" << std::endl;
}

// Print assignment
auto assignment = tradeBus.getCoreAssignment();
if (assignment) {
    std::cout << "Market data cores: ";
    for (int c : assignment->marketDataCores) std::cout << c << " ";
    std::cout << std::endl;
}
```

## 8. Full Example

```cpp
#include "flox/book/bus/trade_bus.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/execution/bus/order_execution_bus.h"

int main()
{
    flox::init_timebase_mapping();

#if FLOX_CPU_AFFINITY_ENABLED
    auto cpuAffinity = flox::performance::createCpuAffinity();

    flox::performance::CriticalComponentConfig config;
    config.preferIsolatedCores = true;
    config.exclusiveIsolatedCores = true;

    auto assignment = cpuAffinity->getNumaAwareCoreAssignment(config);

    if (assignment.hasIsolatedCores) {
        std::cout << "Using isolated cores for critical components" << std::endl;
    }
#endif

    auto tradeBus = std::make_unique<TradeBus>();
    auto bookBus = std::make_unique<BookUpdateBus>();
    auto execBus = std::make_unique<OrderExecutionBus>();

#if FLOX_CPU_AFFINITY_ENABLED
    tradeBus->setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA);
    bookBus->setupOptimalConfiguration(BookUpdateBus::ComponentType::MARKET_DATA);
    execBus->setupOptimalConfiguration(OrderExecutionBus::ComponentType::EXECUTION);
#endif

    // ... rest of setup and run
}
```

## Troubleshooting

### "NUMA library not found"

Install libnuma:
```bash
sudo apt install libnuma-dev  # Debian/Ubuntu
sudo yum install numactl-devel  # RHEL/CentOS
```

### Performance is worse with affinity

- Ensure cores are properly isolated (`cat /sys/devices/system/cpu/isolated`)
- Disable frequency scaling
- Verify you have enough isolated cores for all critical threads
- Check for IRQ affinity conflicts (`cat /proc/interrupts`)

### "Permission denied" setting real-time priority

Run as root or add capability:
```bash
sudo setcap cap_sys_nice+ep ./your_binary
```

Or run with:
```bash
sudo nice -n -20 ./your_binary
```

## See Also

- [Optimize Performance](optimize-performance.md) — More tuning options
- [Architecture Overview](../explanation/architecture.md) — Threading model
