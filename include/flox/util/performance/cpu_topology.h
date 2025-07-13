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
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>
#include "system_interface.h"

namespace flox::performance
{

/**
 * @brief NUMA node information
 */
struct NumaNode
{
  int nodeId;
  std::vector<int> cpuCores;
  size_t totalMemoryMB;
  size_t freeMemoryMB;
};

/**
 * @brief NUMA topology information
 */
struct NumaTopology
{
  std::vector<NumaNode> nodes;
  int numNodes;
  bool numaAvailable;
};

/**
 * @brief CPU topology and NUMA information management
 * 
 * This class provides functionality to:
 * - Get CPU core count and topology information
 * - Retrieve NUMA node information
 * - Identify isolated cores
 * - Map cores to NUMA nodes
 */
class CpuTopology
{
 public:
  /**
     * @brief Constructor with system interface dependency injection
     * @param systemInterface System interface for platform-specific operations
     */
  explicit CpuTopology(std::unique_ptr<ISystemInterface> systemInterface);

  /**
     * @brief Get total number of CPU cores
     * @return Number of CPU cores
     */
  int getNumCores() const;

  /**
     * @brief Get list of isolated CPU cores
     * @return Vector of isolated core IDs
     */
  std::vector<int> getIsolatedCores() const;

  /**
     * @brief Get NUMA topology information
     * @return NUMA topology structure
     */
  NumaTopology getNumaTopology() const;

  /**
     * @brief Get NUMA node for a specific CPU core
     * @param coreId CPU core ID
     * @return NUMA node ID (0 if NUMA not available)
     */
  int getNumaNodeForCore(int coreId) const;

  /**
     * @brief Check if NUMA is available on the system
     * @return true if NUMA is available, false otherwise
     */
  bool isNumaAvailable() const;

  /**
     * @brief Get cores belonging to a specific NUMA node
     * @param nodeId NUMA node ID
     * @return Vector of core IDs belonging to the node
     */
  std::vector<int> getCoresForNumaNode(int nodeId) const;

  /**
     * @brief Get all available CPU cores
     * @return Vector of all core IDs
     */
  std::vector<int> getAllCores() const;

  /**
     * @brief Get non-isolated CPU cores
     * @return Vector of non-isolated core IDs
     */
  std::vector<int> getNonIsolatedCores() const;

 private:
  std::unique_ptr<ISystemInterface> _systemInterface;
  mutable std::optional<NumaTopology> _cachedTopology;
  mutable std::optional<std::vector<int>> _cachedIsolatedCores;
  mutable std::optional<int> _cachedNumCores;

  /**
     * @brief Parse NUMA node memory information
     * @param memInfo Memory information string
     * @param totalMemoryMB Output total memory in MB
     * @param freeMemoryMB Output free memory in MB
     */
  void parseNodeMemInfo(const std::string& memInfo, size_t& totalMemoryMB, size_t& freeMemoryMB) const;

  /**
     * @brief Build NUMA topology from system information
     * @return NUMA topology structure
     */
  NumaTopology buildNumaTopology() const;
};

// Inline implementations
inline CpuTopology::CpuTopology(std::unique_ptr<ISystemInterface> systemInterface)
    : _systemInterface(std::move(systemInterface))
{
}

inline int CpuTopology::getNumCores() const
{
  if (!_cachedNumCores)
  {
    _cachedNumCores = _systemInterface->getNumCores();
  }
  return *_cachedNumCores;
}

inline std::vector<int> CpuTopology::getIsolatedCores() const
{
  if (!_cachedIsolatedCores)
  {
    _cachedIsolatedCores = _systemInterface->getIsolatedCores();
  }
  return *_cachedIsolatedCores;
}

inline NumaTopology CpuTopology::getNumaTopology() const
{
  if (!_cachedTopology)
  {
    _cachedTopology = buildNumaTopology();
  }
  return *_cachedTopology;
}

inline int CpuTopology::getNumaNodeForCore(int coreId) const
{
  return _systemInterface->getNumaNodeForCore(coreId);
}

inline bool CpuTopology::isNumaAvailable() const
{
  return _systemInterface->isNumaAvailable();
}

inline std::vector<int> CpuTopology::getCoresForNumaNode(int nodeId) const
{
  auto topology = getNumaTopology();

  for (const auto& node : topology.nodes)
  {
    if (node.nodeId == nodeId)
    {
      return node.cpuCores;
    }
  }

  return {};
}

inline std::vector<int> CpuTopology::getAllCores() const
{
  const auto numCores = getNumCores();
  std::vector<int> allCores(numCores);
  std::iota(allCores.begin(), allCores.end(), 0);
  return allCores;
}

inline std::vector<int> CpuTopology::getNonIsolatedCores() const
{
  auto allCores = getAllCores();
  auto isolatedCores = getIsolatedCores();

  std::vector<int> nonIsolatedCores;

  for (int core : allCores)
  {
    if (std::find(isolatedCores.begin(), isolatedCores.end(), core) == isolatedCores.end())
    {
      nonIsolatedCores.push_back(core);
    }
  }

  return nonIsolatedCores;
}

inline NumaTopology CpuTopology::buildNumaTopology() const
{
  NumaTopology topology;
  topology.numaAvailable = isNumaAvailable();

  if (!topology.numaAvailable)
  {
    // When NUMA not available, return empty topology to match original behavior
    topology.numNodes = 0;
    return topology;
  }

  // Get NUMA nodes from system interface
  auto numaNodes = _systemInterface->getNumaNodes();

  for (const auto& [nodeId, cores] : numaNodes)
  {
    NumaNode node;
    node.nodeId = nodeId;
    node.cpuCores = cores;

    // Get memory information for this node
    std::string memInfoPath = "/sys/devices/system/node/node" + std::to_string(nodeId) + "/meminfo";
    auto memInfo = _systemInterface->readFile(memInfoPath);

    if (memInfo)
    {
      parseNodeMemInfo(*memInfo, node.totalMemoryMB, node.freeMemoryMB);
    }
    else
    {
      node.totalMemoryMB = 0;
      node.freeMemoryMB = 0;
    }

    topology.nodes.push_back(node);
  }

  topology.numNodes = topology.nodes.size();
  return topology;
}

inline void CpuTopology::parseNodeMemInfo(const std::string& memInfo, size_t& totalMemoryMB, size_t& freeMemoryMB) const
{
  std::stringstream ss(memInfo);
  std::string line;

  totalMemoryMB = 0;
  freeMemoryMB = 0;

  while (std::getline(ss, line))
  {
    if (line.find("MemTotal:") != std::string::npos)
    {
      std::stringstream lineStream(line);
      std::string label, value, unit;
      lineStream >> label >> value >> unit;
      totalMemoryMB = std::stoull(value) / 1024;  // Convert from KB to MB
    }
    else if (line.find("MemFree:") != std::string::npos)
    {
      std::stringstream lineStream(line);
      std::string label, value, unit;
      lineStream >> label >> value >> unit;
      freeMemoryMB = std::stoull(value) / 1024;  // Convert from KB to MB
    }
  }
}

}  // namespace flox::performance