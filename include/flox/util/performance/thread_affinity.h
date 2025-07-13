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
#include <memory>
#include <thread>
#include <vector>
#include "system_interface.h"

namespace flox::performance
{

/**
 * @brief Thread affinity and priority management
 * 
 * This class provides functionality to:
 * - Pin threads to specific CPU cores
 * - Set thread priorities for real-time performance
 * - Manage thread affinity for the current thread
 * - Set NUMA memory policies for threads
 */
class ThreadAffinity
{
 public:
  /**
     * @brief Constructor with system interface dependency injection
     * @param systemInterface System interface for platform-specific operations
     */
  explicit ThreadAffinity(std::unique_ptr<ISystemInterface> systemInterface);

  /**
     * @brief Pin current thread to specific CPU core
     * @param coreId CPU core ID (0-based)
     * @return true if successful, false otherwise
     */
  bool pinCurrentThreadToCore(int coreId);

  /**
     * @brief Pin current thread to multiple CPU cores
     * @param coreIds Vector of CPU core IDs (0-based)
     * @return true if successful, false otherwise
     */
  bool pinCurrentThreadToCores(const std::vector<int>& coreIds);

  /**
     * @brief Pin a thread to specific CPU core
     * @param thread Thread to pin
     * @param coreId CPU core ID (0-based)
     * @return true if successful, false otherwise
     */
  bool pinThreadToCore(std::thread& thread, int coreId);

  /**
     * @brief Pin a thread to multiple CPU cores
     * @param thread Thread to pin
     * @param coreIds Vector of CPU core IDs (0-based)
     * @return true if successful, false otherwise
     */
  bool pinThreadToCores(std::thread& thread, const std::vector<int>& coreIds);

  /**
     * @brief Set thread priority for real-time performance
     * @param priority Priority level (1-99, higher = more priority)
     * @return true if successful, false otherwise
     */
  bool setCurrentThreadPriority(int priority = 80);

  /**
     * @brief Set thread priority for a specific thread
     * @param thread Thread to set priority for
     * @param priority Priority level (1-99, higher = more priority)
     * @return true if successful, false otherwise
     */
  bool setThreadPriority(std::thread& thread, int priority = 80);

  /**
     * @brief Get current thread's CPU affinity
     * @return Vector of CPU core IDs that the thread is bound to
     */
  std::vector<int> getCurrentThreadAffinity();

  /**
     * @brief Set NUMA memory policy for current thread
     * @param nodeId NUMA node ID to prefer for memory allocation
     * @return true if successful, false otherwise
     */
  bool setCurrentThreadNumaPolicy(int nodeId);

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
     * @brief Verify that critical cores are properly isolated
     * @param criticalCores Vector of core IDs that should be isolated
     * @return true if all cores are isolated, false otherwise
     */
  bool verifyCriticalCoreIsolation(const std::vector<int>& criticalCores);

 private:
  std::unique_ptr<ISystemInterface> _systemInterface;

  /**
     * @brief Check if a core is isolated
     * @param coreId Core ID to check
     * @return true if core is isolated, false otherwise
     */
  bool isCoreIsolated(int coreId);

  /**
     * @brief Set CPU governor for frequency scaling
     * @param governor Governor name (e.g., "performance", "powersave")
     * @return true if successful, false otherwise
     */
  bool setCpuGovernor(const std::string& governor);
};

/**
 * @brief RAII guard for thread affinity management
 * 
 * This class automatically restores the original thread affinity when destroyed.
 * Useful for temporary affinity changes in a scope.
 */
class ThreadAffinityGuard
{
 public:
  /**
     * @brief Constructor that pins current thread to a single core
     * @param coreId Core ID to pin to
     */
  explicit ThreadAffinityGuard(int coreId);

  /**
     * @brief Constructor that pins current thread to multiple cores
     * @param coreIds Vector of core IDs to pin to
     */
  explicit ThreadAffinityGuard(const std::vector<int>& coreIds);

  /**
     * @brief Destructor that restores original affinity
     */
  ~ThreadAffinityGuard();

  // Non-copyable, non-movable
  ThreadAffinityGuard(const ThreadAffinityGuard&) = delete;
  ThreadAffinityGuard& operator=(const ThreadAffinityGuard&) = delete;
  ThreadAffinityGuard(ThreadAffinityGuard&&) = delete;
  ThreadAffinityGuard& operator=(ThreadAffinityGuard&&) = delete;

 private:
  std::unique_ptr<ISystemInterface> _systemInterface;
  std::vector<int> _originalAffinity;
  bool validGuard_;
};

// Inline implementations
inline ThreadAffinity::ThreadAffinity(std::unique_ptr<ISystemInterface> systemInterface)
    : _systemInterface(std::move(systemInterface))
{
}

inline bool ThreadAffinity::pinCurrentThreadToCore(int coreId)
{
  return _systemInterface->setCurrentThreadAffinity({coreId});
}

inline bool ThreadAffinity::pinCurrentThreadToCores(const std::vector<int>& coreIds)
{
  return _systemInterface->setCurrentThreadAffinity(coreIds);
}

inline bool ThreadAffinity::pinThreadToCore(std::thread& thread, int coreId)
{
  return _systemInterface->setThreadAffinity(thread.native_handle(), {coreId});
}

inline bool ThreadAffinity::pinThreadToCores(std::thread& thread, const std::vector<int>& coreIds)
{
  return _systemInterface->setThreadAffinity(thread.native_handle(), coreIds);
}

inline bool ThreadAffinity::setCurrentThreadPriority(int priority)
{
  return _systemInterface->setCurrentThreadPriority(priority);
}

inline bool ThreadAffinity::setThreadPriority(std::thread& thread, int priority)
{
  return _systemInterface->setThreadPriority(thread.native_handle(), priority);
}

inline std::vector<int> ThreadAffinity::getCurrentThreadAffinity()
{
  return _systemInterface->getCurrentThreadAffinity();
}

inline bool ThreadAffinity::setCurrentThreadNumaPolicy(int nodeId)
{
  return _systemInterface->setMemoryPolicy(nodeId);
}

inline bool ThreadAffinity::disableCpuFrequencyScaling()
{
  return setCpuGovernor("performance");
}

inline bool ThreadAffinity::enableCpuFrequencyScaling()
{
  return setCpuGovernor("powersave");
}

inline bool ThreadAffinity::verifyCriticalCoreIsolation(const std::vector<int>& criticalCores)
{
  auto isolatedCores = _systemInterface->getIsolatedCores();

  for (int coreId : criticalCores)
  {
    if (!isCoreIsolated(coreId))
    {
      return false;
    }
  }

  return true;
}

inline bool ThreadAffinity::isCoreIsolated(int coreId)
{
  auto isolatedCores = _systemInterface->getIsolatedCores();
  return std::find(isolatedCores.begin(), isolatedCores.end(), coreId) != isolatedCores.end();
}

inline bool ThreadAffinity::setCpuGovernor(const std::string& governor)
{
  const auto numCores = _systemInterface->getNumCores();
  bool success = true;

  for (int i = 0; i < numCores; ++i)
  {
    std::string govPath = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
    if (!_systemInterface->writeFile(govPath, governor))
    {
      success = false;
    }
  }

  return success;
}

// ThreadAffinityGuard implementation
inline ThreadAffinityGuard::ThreadAffinityGuard(int coreId)
    : _systemInterface(createSystemInterface()), validGuard_(false)
{
  _originalAffinity = _systemInterface->getCurrentThreadAffinity();

  if (_systemInterface->setCurrentThreadAffinity({coreId}))
  {
    validGuard_ = true;
  }
}

inline ThreadAffinityGuard::ThreadAffinityGuard(const std::vector<int>& coreIds)
    : _systemInterface(createSystemInterface()), validGuard_(false)
{
  _originalAffinity = _systemInterface->getCurrentThreadAffinity();

  if (_systemInterface->setCurrentThreadAffinity(coreIds))
  {
    validGuard_ = true;
  }
}

inline ThreadAffinityGuard::~ThreadAffinityGuard()
{
  if (validGuard_ && !_originalAffinity.empty())
  {
    _systemInterface->setCurrentThreadAffinity(_originalAffinity);
  }
}

}  // namespace flox::performance