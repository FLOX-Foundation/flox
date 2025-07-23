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
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// NUMA support detection - use CMake-defined flag
#if defined(FLOX_NUMA_LIBRARY_LINKED) && FLOX_NUMA_LIBRARY_LINKED && __has_include(<numaif.h>)
#include <numa.h>
#include <numaif.h>
#define FLOX_NUMA_AVAILABLE 1
#else
#define FLOX_NUMA_AVAILABLE 0
#endif

#else
#define FLOX_NUMA_AVAILABLE 0
#endif

namespace flox::performance
{

/**
 * @brief Abstract interface for system-level operations
 * 
 * This interface abstracts platform-specific system calls for CPU affinity,
 * thread management, and file operations. It allows for easier testing and
 * platform portability.
 */
class ISystemInterface
{
 public:
  virtual ~ISystemInterface() = default;

  // Thread affinity operations
  virtual bool setThreadAffinity(pthread_t thread, const std::vector<int>& cores) = 0;
  virtual bool setCurrentThreadAffinity(const std::vector<int>& cores) = 0;
  virtual std::vector<int> getCurrentThreadAffinity() = 0;

  // Thread priority operations
  virtual bool setThreadPriority(pthread_t thread, int priority) = 0;
  virtual bool setCurrentThreadPriority(int priority) = 0;

  // System information
  virtual int getNumCores() = 0;
  virtual std::vector<int> getIsolatedCores() = 0;

  // File operations
  virtual std::optional<std::string> readFile(const std::string& path) = 0;
  virtual bool writeFile(const std::string& path, const std::string& content) = 0;

  // NUMA operations
  virtual bool isNumaAvailable() = 0;
  virtual bool setMemoryPolicy(int nodeId) = 0;
  virtual int getNumaNodeForCore(int coreId) = 0;
  virtual std::vector<std::pair<int, std::vector<int>>> getNumaNodes() = 0;
};

/**
 * @brief Linux-specific implementation of system interface
 */
class LinuxSystemInterface : public ISystemInterface
{
 public:
  bool setThreadAffinity(pthread_t thread, const std::vector<int>& cores) override;
  bool setCurrentThreadAffinity(const std::vector<int>& cores) override;
  std::vector<int> getCurrentThreadAffinity() override;

  bool setThreadPriority(pthread_t thread, int priority) override;
  bool setCurrentThreadPriority(int priority) override;

  int getNumCores() override;
  std::vector<int> getIsolatedCores() override;

  std::optional<std::string> readFile(const std::string& path) override;
  bool writeFile(const std::string& path, const std::string& content) override;

  bool isNumaAvailable() override;
  bool setMemoryPolicy(int nodeId) override;
  int getNumaNodeForCore(int coreId) override;
  std::vector<std::pair<int, std::vector<int>>> getNumaNodes() override;

 private:
  std::vector<int> parseIntList(const std::string& list);
  void parseNodeMemInfo(const std::string& memInfo, size_t& totalMemoryMB, size_t& freeMemoryMB);
};

/**
 * @brief Null implementation for unsupported platforms
 */
class NullSystemInterface : public ISystemInterface
{
 public:
  bool setThreadAffinity(pthread_t, const std::vector<int>&) override { return false; }
  bool setCurrentThreadAffinity(const std::vector<int>&) override { return false; }
  std::vector<int> getCurrentThreadAffinity() override { return {}; }

  bool setThreadPriority(pthread_t, int) override { return false; }
  bool setCurrentThreadPriority(int) override { return false; }

  int getNumCores() override { return std::thread::hardware_concurrency(); }
  std::vector<int> getIsolatedCores() override { return {}; }

  std::optional<std::string> readFile(const std::string&) override { return std::nullopt; }
  bool writeFile(const std::string&, const std::string&) override { return false; }

  bool isNumaAvailable() override { return false; }
  bool setMemoryPolicy(int) override { return false; }
  int getNumaNodeForCore(int) override { return 0; }
  std::vector<std::pair<int, std::vector<int>>> getNumaNodes() override { return {}; }
};

/**
 * @brief Factory function to create appropriate system interface
 */
std::unique_ptr<ISystemInterface> createSystemInterface();

// Inline implementations
#ifdef __linux__

inline bool LinuxSystemInterface::setThreadAffinity(pthread_t thread, const std::vector<int>& cores)
{
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  for (int core : cores)
  {
    CPU_SET(core, &cpuset);
  }

  return pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) == 0;
}

inline bool LinuxSystemInterface::setCurrentThreadAffinity(const std::vector<int>& cores)
{
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  for (int core : cores)
  {
    CPU_SET(core, &cpuset);
  }

  return sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0;
}

inline std::vector<int> LinuxSystemInterface::getCurrentThreadAffinity()
{
  std::vector<int> affinity;
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) == 0)
  {
    for (int i = 0; i < CPU_SETSIZE; i++)
    {
      if (CPU_ISSET(i, &cpuset))
      {
        affinity.push_back(i);
      }
    }
  }

  return affinity;
}

inline bool LinuxSystemInterface::setThreadPriority(pthread_t thread, int priority)
{
  struct sched_param param;
  param.sched_priority = priority;
  return pthread_setschedparam(thread, SCHED_FIFO, &param) == 0;
}

inline bool LinuxSystemInterface::setCurrentThreadPriority(int priority)
{
  struct sched_param param;
  param.sched_priority = priority;
  return sched_setscheduler(0, SCHED_FIFO, &param) == 0;
}

inline int LinuxSystemInterface::getNumCores()
{
  return std::thread::hardware_concurrency();
}

inline std::vector<int> LinuxSystemInterface::getIsolatedCores()
{
  auto content = readFile("/proc/cmdline");
  if (!content)
  {
    return {};
  }

  std::vector<int> isolatedCores;
  std::stringstream ss(*content);
  std::string token;

  while (std::getline(ss, token, ' '))
  {
    if (token.find("isolcpus=") == 0)
    {
      std::string coreList = token.substr(9);
      isolatedCores = parseIntList(coreList);
      break;
    }
  }

  return isolatedCores;
}

inline std::optional<std::string> LinuxSystemInterface::readFile(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
  {
    return std::nullopt;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

inline bool LinuxSystemInterface::writeFile(const std::string& path, const std::string& content)
{
  std::ofstream file(path);
  if (!file.is_open())
  {
    return false;
  }

  file << content;
  return file.good();
}

inline bool LinuxSystemInterface::isNumaAvailable()
{
#if FLOX_NUMA_AVAILABLE
  // Check if NUMA is actually available using NUMA library
  return numa_available() != -1;
#else
  // If NUMA library not available, check if /sys/devices/system/node exists
  // and has at least one node directory
  auto nodeContent = readFile("/sys/devices/system/node/node0/cpulist");
  if (nodeContent && !nodeContent->empty())
  {
    // Check if there's more than one node (true NUMA)
    auto node1Content = readFile("/sys/devices/system/node/node1/cpulist");
    return node1Content && !node1Content->empty();
  }
  return false;
#endif
}

inline bool LinuxSystemInterface::setMemoryPolicy(int nodeId)
{
#if FLOX_NUMA_AVAILABLE
  if (nodeId < 0)
  {
    return false;
  }

  struct bitmask* nodeMask = numa_allocate_nodemask();
  if (!nodeMask)
  {
    return false;
  }

  numa_bitmask_setbit(nodeMask, nodeId);
  int result = set_mempolicy(MPOL_PREFERRED, nodeMask->maskp, nodeMask->size + 1);
  numa_free_nodemask(nodeMask);

  return result == 0;
#else
  (void)nodeId;
  return false;
#endif
}

inline int LinuxSystemInterface::getNumaNodeForCore(int coreId)
{
  // Return -1 when NUMA is not available to match original behavior
  if (!isNumaAvailable())
  {
    return -1;
  }

  // Check if core exists by checking if it's within valid range
  const auto numCores = getNumCores();
  if (coreId < 0 || coreId >= numCores)
  {
    return -1;  // Return -1 for invalid cores
  }

  std::string nodePath = "/sys/devices/system/cpu/cpu" + std::to_string(coreId) + "/topology/physical_package_id";
  auto content = readFile(nodePath);
  if (!content)
  {
    return -1;  // Return -1 if can't read the node info
  }

  try
  {
    return std::stoi(*content);
  }
  catch (...)
  {
    return -1;  // Return -1 for parsing errors
  }
}

inline std::vector<std::pair<int, std::vector<int>>> LinuxSystemInterface::getNumaNodes()
{
  std::vector<std::pair<int, std::vector<int>>> nodes;

  for (int nodeId = 0; nodeId < 8; ++nodeId)  // Check up to 8 nodes
  {
    std::string nodePath = "/sys/devices/system/node/node" + std::to_string(nodeId);
    std::string cpuListPath = nodePath + "/cpulist";

    auto content = readFile(cpuListPath);
    if (!content)
    {
      continue;
    }

    std::vector<int> cpuCores = parseIntList(*content);
    if (!cpuCores.empty())
    {
      nodes.emplace_back(nodeId, std::move(cpuCores));
    }
  }

  return nodes;
}

inline std::vector<int> LinuxSystemInterface::parseIntList(const std::string& list)
{
  std::vector<int> result;
  std::stringstream ss(list);
  std::string token;

  while (std::getline(ss, token, ','))
  {
    // Remove whitespace
    token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());

    if (token.find('-') != std::string::npos)
    {
      // Range like "0-3"
      size_t dashPos = token.find('-');
      int start = std::stoi(token.substr(0, dashPos));
      int end = std::stoi(token.substr(dashPos + 1));

      for (int i = start; i <= end; ++i)
      {
        result.push_back(i);
      }
    }
    else
    {
      // Single number
      result.push_back(std::stoi(token));
    }
  }

  return result;
}

inline void LinuxSystemInterface::parseNodeMemInfo(const std::string& memInfo, size_t& totalMemoryMB, size_t& freeMemoryMB)
{
  std::stringstream ss(memInfo);
  std::string line;

  totalMemoryMB = 0;
  freeMemoryMB = 0;

  while (std::getline(ss, line))
  {
    if (line.find("MemTotal:") == 0)
    {
      std::stringstream lineStream(line);
      std::string label, value, unit;
      lineStream >> label >> value >> unit;
      totalMemoryMB = std::stoull(value) / 1024;  // Convert from KB to MB
    }
    else if (line.find("MemFree:") == 0)
    {
      std::stringstream lineStream(line);
      std::string label, value, unit;
      lineStream >> label >> value >> unit;
      freeMemoryMB = std::stoull(value) / 1024;  // Convert from KB to MB
    }
  }
}

#endif  // __linux__

inline std::unique_ptr<ISystemInterface> createSystemInterface()
{
#ifdef __linux__
  return std::make_unique<LinuxSystemInterface>();
#else
  return std::make_unique<NullSystemInterface>();
#endif
}

}  // namespace flox::performance