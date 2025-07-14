/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <memory>
#include <thread>

#include "flox/util/performance/core_assignment.h"
#include "flox/util/performance/cpu_topology.h"
#include "flox/util/performance/system_interface.h"
#include "flox/util/performance/thread_affinity.h"

namespace flox::performance
{

/**
 * @brief CPU affinity and thread pinning utilities for low-latency performance optimization
 * 
 * This class provides a high-level interface for CPU affinity management,
 * acting as a facade for the underlying specialized classes:
 * - SystemInterface: Platform-specific operations
 * - CpuTopology: CPU and NUMA topology information
 * - ThreadAffinity: Thread pinning and priority management
 * - CoreAssignmentManager: Core assignment strategies
 * 
 * The class provides better testability and separation of concerns through
 * dependency injection.
 */
class CpuAffinity
{
 public:
  /**
   * @brief Constructor with dependency injection for testing
   * @param systemInterface System interface for platform operations
   * @param cpuTopology CPU topology information provider
   * @param threadAffinity Thread affinity manager
   * @param coreAssignmentManager Core assignment strategy manager
   */
  CpuAffinity(std::unique_ptr<ISystemInterface> systemInterface = nullptr,
              std::shared_ptr<CpuTopology> cpuTopology = nullptr,
              std::unique_ptr<ThreadAffinity> threadAffinity = nullptr,
              std::unique_ptr<CoreAssignmentManager> coreAssignmentManager = nullptr);

  /**
   * @brief Pin current thread to specific CPU core
   * @param coreId CPU core ID (0-based)
   * @return true if successful, false otherwise
   */
  bool pinToCore(int coreId);

  /**
   * @brief Pin a thread to specific CPU core
   * @param thread Thread to pin
   * @param coreId CPU core ID (0-based)
   * @return true if successful, false otherwise
   */
  bool pinToCore(std::thread& thread, int coreId);

  /**
   * @brief Set thread priority for real-time performance
   * @param priority Priority level (1-99, higher = more priority)
   * @return true if successful, false otherwise
   */
  bool setRealTimePriority(int priority = 80);

  /**
   * @brief Set thread priority for a specific thread
   * @param thread Thread to set priority for
   * @param priority Priority level (1-99, higher = more priority)
   * @return true if successful, false otherwise
   */
  bool setRealTimePriority(std::thread& thread, int priority = 80);

  /**
   * @brief Get total number of CPU cores
   * @return Number of CPU cores
   */
  int getNumCores();

  /**
   * @brief Get list of isolated CPU cores
   * @return Vector of isolated core IDs
   */
  std::vector<int> getIsolatedCores();

  /**
   * @brief Get current thread's CPU affinity
   * @return Vector of CPU core IDs that the thread is bound to
   */
  std::vector<int> getCurrentAffinity();

  /**
   * @brief Disable CPU frequency scaling for better performance consistency
   * @return true if successful, false otherwise
   */
  bool disableCpuFrequencyScaling();

  /**
   * @brief Enable CPU frequency scaling
   * @return true if successful, false otherwise
   */
  bool enableCpuFrequencyScaling();

  /**
   * @brief Get recommended core assignment based on system configuration
   * @param config Configuration for critical component assignment
   * @return Optimal core assignment for the system
   */
  CoreAssignment getRecommendedCoreAssignment(const CriticalComponentConfig& config = CriticalComponentConfig{});

  /**
   * @brief Get basic core assignment for systems without special requirements
   * @param numCores Total number of cores to assign
   * @param isolatedCores Vector of isolated core IDs
   * @return Basic core assignment
   */
  CoreAssignment getBasicCoreAssignment(int numCores, const std::vector<int>& isolatedCores);

  /**
   * @brief Get NUMA-aware core assignment for optimal performance
   * @param config Configuration for critical component assignment
   * @return NUMA-optimized core assignment
   */
  CoreAssignment getNumaAwareCoreAssignment(const CriticalComponentConfig& config = CriticalComponentConfig{});

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
   * @brief Get NUMA topology information
   * @return NUMA topology structure
   */
  NumaTopology getNumaTopology();

  /**
   * @brief Get NUMA node for a specific CPU core
   * @param coreId CPU core ID
   * @return NUMA node ID (0 if NUMA not available)
   */
  int getNumaNodeForCore(int coreId);

  /**
   * @brief Pin current thread to a specific NUMA node
   * @param nodeId NUMA node ID
   * @return true if successful, false otherwise
   */
  bool pinToNumaNode(int nodeId);

  /**
   * @brief Set NUMA memory policy for current thread
   * @param nodeId NUMA node ID to set policy for
   * @return true if successful, false otherwise
   */
  bool setMemoryPolicy(int nodeId);

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

  /**
   * @brief Get CPU topology information
   * @return CPU topology manager
   */
  std::shared_ptr<CpuTopology> getCpuTopology() const;

  /**
   * @brief Get thread affinity manager
   * @return Thread affinity manager
   */
  ThreadAffinity& getThreadAffinity() const;

  /**
   * @brief Get core assignment manager
   * @return Core assignment manager
   */
  CoreAssignmentManager& getCoreAssignmentManager() const;

 private:
  std::unique_ptr<ISystemInterface> _systemInterface;
  std::shared_ptr<CpuTopology> _cpuTopology;
  std::unique_ptr<ThreadAffinity> _threadAffinity;
  std::unique_ptr<CoreAssignmentManager> _coreAssignmentManager;
};

// Convenience factory function for creating CpuAffinity instances
inline std::unique_ptr<CpuAffinity> createCpuAffinity()
{
  return std::make_unique<CpuAffinity>();
}

// Inline implementations
inline CpuAffinity::CpuAffinity(std::unique_ptr<ISystemInterface> systemInterface,
                                std::shared_ptr<CpuTopology> cpuTopology,
                                std::unique_ptr<ThreadAffinity> threadAffinity,
                                std::unique_ptr<CoreAssignmentManager> coreAssignmentManager)
{
  // Create default implementations if not provided
  if (!systemInterface)
  {
    _systemInterface = createSystemInterface();
  }
  else
  {
    _systemInterface = std::move(systemInterface);
  }

  if (!cpuTopology)
  {
    _cpuTopology = std::make_shared<CpuTopology>(createSystemInterface());
  }
  else
  {
    _cpuTopology = std::move(cpuTopology);
  }

  if (!threadAffinity)
  {
    _threadAffinity = std::make_unique<ThreadAffinity>(createSystemInterface());
  }
  else
  {
    _threadAffinity = std::move(threadAffinity);
  }

  if (!coreAssignmentManager)
  {
    _coreAssignmentManager = std::make_unique<CoreAssignmentManager>(_cpuTopology);
  }
  else
  {
    _coreAssignmentManager = std::move(coreAssignmentManager);
  }
}

inline bool CpuAffinity::pinToCore(int coreId)
{
  return _threadAffinity->pinCurrentThreadToCore(coreId);
}

inline bool CpuAffinity::pinToCore(std::thread& thread, int coreId)
{
  return _threadAffinity->pinThreadToCore(thread, coreId);
}

inline bool CpuAffinity::setRealTimePriority(int priority)
{
  return _threadAffinity->setCurrentThreadPriority(priority);
}

inline bool CpuAffinity::setRealTimePriority(std::thread& thread, int priority)
{
  return _threadAffinity->setThreadPriority(thread, priority);
}

inline int CpuAffinity::getNumCores()
{
  return _cpuTopology->getNumCores();
}

inline std::vector<int> CpuAffinity::getIsolatedCores()
{
  return _cpuTopology->getIsolatedCores();
}

inline std::vector<int> CpuAffinity::getCurrentAffinity()
{
  return _threadAffinity->getCurrentThreadAffinity();
}

inline bool CpuAffinity::disableCpuFrequencyScaling()
{
  return _threadAffinity->disableCpuFrequencyScaling();
}

inline bool CpuAffinity::enableCpuFrequencyScaling()
{
  return _threadAffinity->enableCpuFrequencyScaling();
}

inline CoreAssignment CpuAffinity::getRecommendedCoreAssignment(const CriticalComponentConfig& config)
{
  return _coreAssignmentManager->getRecommendedCoreAssignment(config);
}

inline CoreAssignment CpuAffinity::getBasicCoreAssignment(int numCores, const std::vector<int>& isolatedCores)
{
  return _coreAssignmentManager->getBasicCoreAssignment(numCores, isolatedCores);
}

inline CoreAssignment CpuAffinity::getNumaAwareCoreAssignment(const CriticalComponentConfig& config)
{
  return _coreAssignmentManager->getNumaAwareCoreAssignment(config);
}

inline bool CpuAffinity::pinCriticalComponent(const std::string& component, const CoreAssignment& assignment)
{
  return _coreAssignmentManager->pinCriticalComponent(component, assignment);
}

inline bool CpuAffinity::verifyCriticalCoreIsolation(const CoreAssignment& assignment)
{
  return _coreAssignmentManager->verifyCriticalCoreIsolation(assignment);
}

inline NumaTopology CpuAffinity::getNumaTopology()
{
  return _cpuTopology->getNumaTopology();
}

inline int CpuAffinity::getNumaNodeForCore(int coreId)
{
  return _cpuTopology->getNumaNodeForCore(coreId);
}

inline bool CpuAffinity::pinToNumaNode(int nodeId)
{
  return _threadAffinity->setCurrentThreadNumaPolicy(nodeId);
}

inline bool CpuAffinity::setMemoryPolicy(int nodeId)
{
  return _threadAffinity->setCurrentThreadNumaPolicy(nodeId);
}

inline bool CpuAffinity::setupAndPinCriticalComponents(const CriticalComponentConfig& config)
{
  return _coreAssignmentManager->setupAndPinCriticalComponents(config);
}

inline bool CpuAffinity::checkIsolatedCoreRequirements(int minRequiredCores)
{
  return _coreAssignmentManager->checkIsolatedCoreRequirements(minRequiredCores);
}

inline void CpuAffinity::demonstrateIsolatedCoreUsage()
{
  _coreAssignmentManager->demonstrateIsolatedCoreUsage();
}

inline std::shared_ptr<CpuTopology> CpuAffinity::getCpuTopology() const
{
  return _cpuTopology;
}

inline ThreadAffinity& CpuAffinity::getThreadAffinity() const
{
  return *_threadAffinity;
}

inline CoreAssignmentManager& CpuAffinity::getCoreAssignmentManager() const
{
  return *_coreAssignmentManager;
}

/**
 * @brief RAII wrapper for NUMA-aware thread affinity and memory policy management
 */
class NumaAffinityGuard
{
 public:
  explicit NumaAffinityGuard(CpuAffinity& cpuAffinity, int numaNodeId)
      : _cpuAffinity(cpuAffinity), _numaNodeId(numaNodeId)
  {
    _originalAffinity = _cpuAffinity.getCurrentAffinity();
    _cpuAffinity.pinToNumaNode(numaNodeId);
    _cpuAffinity.setMemoryPolicy(numaNodeId);
  }

  explicit NumaAffinityGuard(CpuAffinity& cpuAffinity, int coreId, int numaNodeId)
      : _cpuAffinity(cpuAffinity), _numaNodeId(numaNodeId)
  {
    _originalAffinity = _cpuAffinity.getCurrentAffinity();
    _cpuAffinity.pinToCore(coreId);
    _cpuAffinity.setMemoryPolicy(numaNodeId);
  }

  ~NumaAffinityGuard()
  {
    if (_restored || _originalAffinity.empty())
    {
      return;
    }

#ifdef __linux__
    // Restore original CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int coreId : _originalAffinity)
    {
      CPU_SET(coreId, &cpuset);
    }

    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

#if FLOX_NUMA_AVAILABLE
    // Reset memory policy to default
    set_mempolicy(MPOL_DEFAULT, nullptr, 0);
#endif
#endif

    _restored = true;
  }

  NumaAffinityGuard(const NumaAffinityGuard&) = delete;
  NumaAffinityGuard& operator=(const NumaAffinityGuard&) = delete;

 private:
  CpuAffinity& _cpuAffinity;
  std::vector<int> _originalAffinity;
  int _numaNodeId;
  bool _restored = false;
};

}  // namespace flox::performance