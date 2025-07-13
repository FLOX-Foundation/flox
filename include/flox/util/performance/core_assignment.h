/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "cpu_topology.h"
#include "thread_affinity.h"

namespace flox::performance
{

constexpr std::string_view MARKET_DATA_COMPONENT = "marketData";
constexpr std::string_view EXECUTION_COMPONENT = "execution";
constexpr std::string_view STRATEGY_COMPONENT = "strategy";
constexpr std::string_view RISK_COMPONENT = "risk";

/**
 * @brief Core assignment for different system components
 */
struct CoreAssignment
{
  std::vector<int> marketDataCores;  // Cores for market data processing
  std::vector<int> strategyCores;    // Cores for strategy execution
  std::vector<int> executionCores;   // Cores for order execution
  std::vector<int> riskCores;        // Cores for risk management
  std::vector<int> generalCores;     // Cores for general tasks

  // Additional information about isolation
  bool hasIsolatedCores = false;
  std::vector<int> allIsolatedCores;
  std::vector<int> criticalCores;  // All cores assigned to critical tasks
};

/**
 * @brief Configuration for critical component core assignment
 */
struct CriticalComponentConfig
{
  bool preferIsolatedCores;       // Prefer isolated cores for critical tasks
  bool exclusiveIsolatedCores;    // Use isolated cores exclusively for critical tasks
  bool allowSharedCriticalCores;  // Allow multiple critical tasks on same core
  int minIsolatedForCritical;     // Minimum isolated cores needed to use them

  // Priority order for critical component assignment (0 = highest priority)
  std::map<std::string, int> componentPriority;

  // Default constructor with sensible defaults
  CriticalComponentConfig()
      : preferIsolatedCores(true), exclusiveIsolatedCores(true), allowSharedCriticalCores(false), minIsolatedForCritical(1), componentPriority({{std::string(MARKET_DATA_COMPONENT), 0}, {std::string(EXECUTION_COMPONENT), 1}, {std::string(STRATEGY_COMPONENT), 2}, {std::string(RISK_COMPONENT), 3}})
  {
  }
};

/**
 * @brief Core assignment strategy and management
 * 
 * This class provides functionality to:
 * - Generate optimal core assignments for different system components
 * - Consider NUMA topology for performance optimization
 * - Handle isolated cores for critical tasks
 * - Provide different assignment strategies based on system configuration
 */
class CoreAssignmentManager
{
 public:
  /**
     * @brief Constructor with CPU topology dependency injection
     * @param cpuTopology CPU topology information provider
     */
  explicit CoreAssignmentManager(std::shared_ptr<CpuTopology> cpuTopology);

  /**
     * @brief Get recommended core assignment based on system configuration
     * @param config Configuration for critical component assignment
     * @return Optimal core assignment for the system
     */
  CoreAssignment getRecommendedCoreAssignment(const CriticalComponentConfig& config = CriticalComponentConfig{});

  /**
     * @brief Get NUMA-aware core assignment for optimal performance
     * @param config Configuration for critical component assignment
     * @return NUMA-optimized core assignment
     */
  CoreAssignment getNumaAwareCoreAssignment(const CriticalComponentConfig& config = CriticalComponentConfig{});

  /**
     * @brief Get basic core assignment for systems without special requirements
     * @param numCores Total number of cores to assign
     * @param isolatedCores Vector of isolated core IDs
     * @return Basic core assignment
     */
  CoreAssignment getBasicCoreAssignment(int numCores, const std::vector<int>& isolatedCores);

  /**
     * @brief Pin a critical component to its assigned cores
     * @param component Component name ("marketData", "execution", "strategy", "risk")
     * @param assignment Core assignment containing component-specific cores
     * @return true if successful, false otherwise
     */
  bool pinCriticalComponent(const std::string& component, const CoreAssignment& assignment);

  /**
     * @brief Verify that critical cores are properly isolated
     * @param assignment Core assignment to verify
     * @return true if all critical cores are isolated, false otherwise
     */
  bool verifyCriticalCoreIsolation(const CoreAssignment& assignment);

  /**
     * @brief Setup and pin all critical components based on configuration
     * @param config Configuration for critical component assignment
     * @return true if all components were successfully pinned, false otherwise
     */
  bool setupAndPinCriticalComponents(const CriticalComponentConfig& config = CriticalComponentConfig{});

  /**
     * @brief Check if system meets minimum isolated core requirements
     * @param minRequiredCores Minimum number of isolated cores required
     * @return true if requirements are met, false otherwise
     */
  bool checkIsolatedCoreRequirements(int minRequiredCores = 4);

  /**
     * @brief Demonstrate isolated core usage with current system configuration
     */
  void demonstrateIsolatedCoreUsage();

 private:
  std::shared_ptr<CpuTopology> _cpuTopology;

  /**
     * @brief Distribute cores among components based on priority
     * @param availableCores Vector of available core IDs
     * @param config Configuration for component assignment
     * @return Core assignment with cores distributed among components
     */
  CoreAssignment distributeCores(const std::vector<int>& availableCores, const CriticalComponentConfig& config);

  /**
     * @brief Assign cores to a specific component
     * @param assignment Core assignment to modify
     * @param component Component name
     * @param cores Vector of core IDs to assign
     */
  void assignCoresTo(CoreAssignment& assignment, const std::string& component, const std::vector<int>& cores);

  /**
     * @brief Get cores assigned to a specific component
     * @param assignment Core assignment to query
     * @param component Component name
     * @return Vector of core IDs assigned to the component
     */
  std::vector<int> getCoresFor(const CoreAssignment& assignment, const std::string& component);

  /**
     * @brief Balance cores across NUMA nodes for optimal performance
     * @param assignment Core assignment to balance
     * @param topology NUMA topology information
     * @return Balanced core assignment
     */
  CoreAssignment balanceAcrossNumaNodes(const CoreAssignment& assignment, const NumaTopology& topology);

  /**
     * @brief Optimize core assignment for cache locality
     * @param assignment Core assignment to optimize
     * @return Optimized core assignment
     */
  CoreAssignment optimizeForCacheLocality(const CoreAssignment& assignment);
};

// Inline implementations
inline CoreAssignmentManager::CoreAssignmentManager(std::shared_ptr<CpuTopology> cpuTopology)
    : _cpuTopology(std::move(cpuTopology))
{
}

inline CoreAssignment CoreAssignmentManager::getRecommendedCoreAssignment(const CriticalComponentConfig& config)
{
  const auto numCores = _cpuTopology->getNumCores();
  auto isolatedCores = _cpuTopology->getIsolatedCores();

  // If we have enough isolated cores and prefer them, use NUMA-aware assignment
  if (config.preferIsolatedCores && isolatedCores.size() >= config.minIsolatedForCritical)
  {
    return getNumaAwareCoreAssignment(config);
  }

  // Otherwise, use basic assignment
  return getBasicCoreAssignment(numCores, isolatedCores);
}

inline CoreAssignment CoreAssignmentManager::getNumaAwareCoreAssignment(const CriticalComponentConfig& config)
{
  CoreAssignment assignment;
  auto topology = _cpuTopology->getNumaTopology();
  auto isolatedCores = _cpuTopology->getIsolatedCores();

  assignment.hasIsolatedCores = !isolatedCores.empty();
  assignment.allIsolatedCores = isolatedCores;

  // Sort components by priority
  std::vector<std::pair<std::string, int>> sortedComponents;
  for (const auto& [component, priority] : config.componentPriority)
  {
    sortedComponents.emplace_back(component, priority);
  }
  std::sort(sortedComponents.begin(), sortedComponents.end(),
            [](const auto& a, const auto& b)
            { return a.second < b.second; });

  std::vector<int> availableIsolated = isolatedCores;
  auto availableNonIsolated = _cpuTopology->getNonIsolatedCores();

  // Assign cores to critical components based on priority
  for (const auto& [component, priority] : sortedComponents)
  {
    std::vector<int> assignedCores;

    if (config.exclusiveIsolatedCores && !availableIsolated.empty())
    {
      // Use isolated cores exclusively for critical tasks
      int coresToAssign = std::min(static_cast<int>(availableIsolated.size()), 1);

      for (int i = 0; i < coresToAssign; ++i)
      {
        assignedCores.push_back(availableIsolated[i]);
      }

      availableIsolated.erase(availableIsolated.begin(),
                              availableIsolated.begin() + coresToAssign);
    }
    else if (!availableNonIsolated.empty())
    {
      // Use non-isolated cores
      int coresToAssign = std::min(static_cast<int>(availableNonIsolated.size()), 1);

      for (int i = 0; i < coresToAssign; ++i)
      {
        assignedCores.push_back(availableNonIsolated[i]);
      }

      availableNonIsolated.erase(availableNonIsolated.begin(),
                                 availableNonIsolated.begin() + coresToAssign);
    }

    assignCoresTo(assignment, component, assignedCores);
  }

  // Assign remaining cores to general tasks
  for (int core : availableIsolated)
  {
    assignment.generalCores.push_back(core);
  }
  for (int core : availableNonIsolated)
  {
    assignment.generalCores.push_back(core);
  }

  // Build list of critical cores
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.marketDataCores.begin(), assignment.marketDataCores.end());
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.executionCores.begin(), assignment.executionCores.end());
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.strategyCores.begin(), assignment.strategyCores.end());
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.riskCores.begin(), assignment.riskCores.end());

  return balanceAcrossNumaNodes(assignment, topology);
}

inline CoreAssignment CoreAssignmentManager::getBasicCoreAssignment(int numCores, const std::vector<int>& isolatedCores)
{
  CoreAssignment assignment;
  assignment.hasIsolatedCores = !isolatedCores.empty();
  assignment.allIsolatedCores = isolatedCores;

  std::vector<int> availableCores;
  for (int i = 0; i < numCores; ++i)
  {
    availableCores.push_back(i);
  }

  // Simple assignment strategy
  int coreIndex = 0;

  // Market data gets first core
  if (coreIndex < numCores)
  {
    assignment.marketDataCores.push_back(coreIndex++);
  }

  // Execution gets next core
  if (coreIndex < numCores)
  {
    assignment.executionCores.push_back(coreIndex++);
  }

  // Strategy gets next core
  if (coreIndex < numCores)
  {
    assignment.strategyCores.push_back(coreIndex++);
  }

  // Risk gets next core
  if (coreIndex < numCores)
  {
    assignment.riskCores.push_back(coreIndex++);
  }

  // Remaining cores go to general
  for (int i = coreIndex; i < numCores; ++i)
  {
    assignment.generalCores.push_back(i);
  }

  // Build list of critical cores
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.marketDataCores.begin(), assignment.marketDataCores.end());
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.executionCores.begin(), assignment.executionCores.end());
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.strategyCores.begin(), assignment.strategyCores.end());
  assignment.criticalCores.insert(assignment.criticalCores.end(),
                                  assignment.riskCores.begin(), assignment.riskCores.end());

  return assignment;
}

inline bool CoreAssignmentManager::pinCriticalComponent(const std::string& component, const CoreAssignment& assignment)
{
  auto cores = getCoresFor(assignment, component);
  if (cores.empty())
  {
    return false;
  }

  // Create thread affinity manager and pin current thread
  auto systemInterface = createSystemInterface();
  ThreadAffinity threadAffinity(std::move(systemInterface));

  return threadAffinity.pinCurrentThreadToCores(cores);
}

inline bool CoreAssignmentManager::verifyCriticalCoreIsolation(const CoreAssignment& assignment)
{
  auto isolatedCores = _cpuTopology->getIsolatedCores();

  for (int coreId : assignment.criticalCores)
  {
    if (std::find(isolatedCores.begin(), isolatedCores.end(), coreId) == isolatedCores.end())
    {
      return false;
    }
  }

  return true;
}

inline bool CoreAssignmentManager::setupAndPinCriticalComponents(const CriticalComponentConfig& config)
{
  auto assignment = getNumaAwareCoreAssignment(config);

  if (!verifyCriticalCoreIsolation(assignment))
  {
    std::cerr << "Warning: Not all critical cores are isolated" << std::endl;
  }

  // Set up thread affinity for all components
  bool success = true;

  for (const auto& [component, priority] : config.componentPriority)
  {
    if (!pinCriticalComponent(component, assignment))
    {
      std::cerr << "Failed to pin component: " << component << std::endl;
      success = false;
    }
  }

  return success;
}

inline bool CoreAssignmentManager::checkIsolatedCoreRequirements(int minRequiredCores)
{
  auto isolatedCores = _cpuTopology->getIsolatedCores();
  return isolatedCores.size() >= minRequiredCores;
}

inline void CoreAssignmentManager::demonstrateIsolatedCoreUsage()
{
  std::cout << "=== CPU Affinity and Isolated Core Usage Demonstration ===" << std::endl;

  const auto numCores = _cpuTopology->getNumCores();
  auto isolatedCores = _cpuTopology->getIsolatedCores();

  std::cout << "Total CPU cores: " << numCores << std::endl;
  std::cout << "Isolated cores: ";
  for (int core : isolatedCores)
  {
    std::cout << core << " ";
  }
  std::cout << std::endl;

  CriticalComponentConfig config;
  auto assignment = getNumaAwareCoreAssignment(config);

  std::cout << "\nRecommended core assignment:" << std::endl;
  std::cout << "Market Data cores: ";
  for (int core : assignment.marketDataCores)
  {
    std::cout << core << " ";
  }
  std::cout << std::endl;

  std::cout << "Execution cores: ";
  for (int core : assignment.executionCores)
  {
    std::cout << core << " ";
  }
  std::cout << std::endl;

  std::cout << "Strategy cores: ";
  for (int core : assignment.strategyCores)
  {
    std::cout << core << " ";
  }
  std::cout << std::endl;

  std::cout << "Risk cores: ";
  for (int core : assignment.riskCores)
  {
    std::cout << core << " ";
  }
  std::cout << std::endl;

  std::cout << "General cores: ";
  for (int core : assignment.generalCores)
  {
    std::cout << core << " ";
  }
  std::cout << std::endl;
}

inline CoreAssignment CoreAssignmentManager::distributeCores(const std::vector<int>& availableCores, const CriticalComponentConfig& config)
{
  CoreAssignment assignment;

  std::vector<int> cores = availableCores;
  int coreIndex = 0;

  // Sort components by priority
  std::vector<std::pair<std::string, int>> sortedComponents;
  for (const auto& [component, priority] : config.componentPriority)
  {
    sortedComponents.emplace_back(component, priority);
  }
  std::sort(sortedComponents.begin(), sortedComponents.end(),
            [](const auto& a, const auto& b)
            { return a.second < b.second; });

  // Assign cores based on priority
  for (const auto& [component, priority] : sortedComponents)
  {
    int coresToAssign = 1;  // All components get 1 core each
    std::vector<int> assignedCores;

    for (int i = 0; i < coresToAssign && coreIndex < cores.size(); ++i, ++coreIndex)
    {
      assignedCores.push_back(cores[coreIndex]);
    }

    assignCoresTo(assignment, component, assignedCores);
  }

  // Remaining cores go to general
  for (int i = coreIndex; i < cores.size(); ++i)
  {
    assignment.generalCores.push_back(cores[i]);
  }

  return assignment;
}

inline void CoreAssignmentManager::assignCoresTo(CoreAssignment& assignment, const std::string& component, const std::vector<int>& cores)
{
  if (component == MARKET_DATA_COMPONENT)
  {
    assignment.marketDataCores = cores;
  }
  else if (component == EXECUTION_COMPONENT)
  {
    assignment.executionCores = cores;
  }
  else if (component == STRATEGY_COMPONENT)
  {
    assignment.strategyCores = cores;
  }
  else if (component == RISK_COMPONENT)
  {
    assignment.riskCores = cores;
  }
}

inline std::vector<int> CoreAssignmentManager::getCoresFor(const CoreAssignment& assignment, const std::string& component)
{
  if (component == MARKET_DATA_COMPONENT)
  {
    return assignment.marketDataCores;
  }
  else if (component == EXECUTION_COMPONENT)
  {
    return assignment.executionCores;
  }
  else if (component == STRATEGY_COMPONENT)
  {
    return assignment.strategyCores;
  }
  else if (component == RISK_COMPONENT)
  {
    return assignment.riskCores;
  }

  return {};
}

inline CoreAssignment CoreAssignmentManager::balanceAcrossNumaNodes(const CoreAssignment& assignment, const NumaTopology& topology)
{
  if (!topology.numaAvailable || topology.nodes.size() <= 1)
  {
    return assignment;
  }

  // For now, return assignment as-is
  // In a full implementation, we would balance cores across NUMA nodes
  // based on workload characteristics and memory access patterns
  return assignment;
}

inline CoreAssignment CoreAssignmentManager::optimizeForCacheLocality(const CoreAssignment& assignment)
{
  // For now, return assignment as-is
  // In a full implementation, we would optimize core assignment
  // based on cache hierarchy and core topology
  return assignment;
}

}  // namespace flox::performance