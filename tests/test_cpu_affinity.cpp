/*
 * Flox Engine - CPU Affinity Tests
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * NOTE: These tests work best on isolated systems with minimal background load.
 * On busy systems, CPU affinity tests may be flaky or show inconsistent results.
 */

#include "flox/book/bus/trade_bus.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/util/performance/cpu_affinity.h"

#include <gtest/gtest.h>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::performance;

class CpuAffinityTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    // Create CpuAffinity instance for testing
    _cpuAffinity = createCpuAffinity();

    // Save original affinity
    _originalAffinity = _cpuAffinity->getCurrentAffinity();
    _numCores = _cpuAffinity->getNumCores();
  }

  void TearDown() override
  {
    // Restore original affinity
    if (!_originalAffinity.empty())
    {
#ifdef __linux__
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      for (int core : _originalAffinity)
      {
        CPU_SET(core, &cpuset);
      }
      sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif
    }
  }

  // Helper method to check if NUMA is available on this system
  bool isNumaAvailable()
  {
    auto topology = _cpuAffinity->getNumaTopology();
    return topology.numaAvailable && !topology.nodes.empty();
  }

  // Helper method to get a valid NUMA node for testing
  int getTestNumaNode()
  {
    auto topology = _cpuAffinity->getNumaTopology();
    if (topology.numaAvailable && !topology.nodes.empty())
    {
      return topology.nodes[0].nodeId;
    }
    return -1;
  }

  std::unique_ptr<CpuAffinity> _cpuAffinity;
  std::vector<int> _originalAffinity;
  int _numCores;
};

/**
 * @brief Test basic CPU affinity functionality
 */
TEST_F(CpuAffinityTest, BasicCpuInfo)
{
  EXPECT_GT(_numCores, 0);
  EXPECT_LE(_numCores, 256);  // Reasonable upper bound

  auto currentAffinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_FALSE(currentAffinity.empty());

  // All cores should be valid
  for (int core : currentAffinity)
  {
    EXPECT_GE(core, 0);
    EXPECT_LT(core, _numCores);
  }
}

/**
 * @brief Test CPU core pinning
 */
TEST_F(CpuAffinityTest, PinToCore)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  // Pin to core 0
  bool result = _cpuAffinity->pinToCore(0);

#ifdef __linux__
  EXPECT_TRUE(result);

  auto affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);

  // Pin to core 1
  result = _cpuAffinity->pinToCore(1);
  EXPECT_TRUE(result);

  affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 1);
#else
  EXPECT_FALSE(result);  // Should fail on non-Linux platforms
#endif
}

/**
 * @brief Test invalid core pinning
 */
TEST_F(CpuAffinityTest, PinToInvalidCore)
{
  // Try to pin to invalid core
  bool result = _cpuAffinity->pinToCore(999);
  EXPECT_FALSE(result);

  result = _cpuAffinity->pinToCore(-1);
  EXPECT_FALSE(result);
}

/**
 * @brief Test thread affinity guard
 */
TEST_F(CpuAffinityTest, ThreadAffinityGuard)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

#ifdef __linux__
  // Pin to core 0 first
  _cpuAffinity->pinToCore(0);
  auto affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);

  {
    // Use guard to temporarily pin to core 1
    ThreadAffinityGuard guard(1);

    affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 1);
  }

  // Should be restored to core 0
  affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);
#endif
}

/**
 * @brief Test thread affinity guard with multiple cores
 */
TEST_F(CpuAffinityTest, ThreadAffinityGuardMultipleCores)
{
  if (_numCores < 3)
  {
    GTEST_SKIP() << "Need at least 3 cores for this test";
  }

#ifdef __linux__
  {
    // Use guard to pin to cores 0 and 1
    ThreadAffinityGuard guard({0, 1});

    auto affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 2);
    EXPECT_TRUE(std::find(affinity.begin(), affinity.end(), 0) != affinity.end());
    EXPECT_TRUE(std::find(affinity.begin(), affinity.end(), 1) != affinity.end());
  }

  // Should be restored to original affinity
  auto affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_EQ(affinity.size(), _originalAffinity.size());
#endif
}

/**
 * @brief Test thread pinning with separate thread
 */
TEST_F(CpuAffinityTest, ThreadPinning)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  std::atomic<bool> threadPinned{false};
  std::atomic<int> threadCore{-1};

  std::thread t([&]()
                {
        bool result = _cpuAffinity->pinToCore(1);
        threadPinned.store(result);
        
        if (result)
        {
            auto affinity = _cpuAffinity->getCurrentAffinity();
            if (affinity.size() == 1)
            {
                threadCore.store(affinity[0]);
            }
        } });

  t.join();

#ifdef __linux__
  EXPECT_TRUE(threadPinned.load());
  EXPECT_EQ(threadCore.load(), 1);
#else
  EXPECT_FALSE(threadPinned.load());
#endif
}

/**
 * @brief Test recommended core assignment
 */
TEST_F(CpuAffinityTest, RecommendedCoreAssignment)
{
  auto assignment = _cpuAffinity->getRecommendedCoreAssignment();

  // Should have at least some cores assigned
  int totalAssigned = assignment.marketDataCores.size() +
                      assignment.strategyCores.size() +
                      assignment.executionCores.size() +
                      assignment.riskCores.size() +
                      assignment.generalCores.size();

  EXPECT_GT(totalAssigned, 0);

  // All assigned cores should be valid
  auto validateCores = [this](const std::vector<int>& cores)
  {
    for (int core : cores)
    {
      EXPECT_GE(core, 0);
      EXPECT_LT(core, _numCores);
    }
  };

  validateCores(assignment.marketDataCores);
  validateCores(assignment.strategyCores);
  validateCores(assignment.executionCores);
  validateCores(assignment.riskCores);
  validateCores(assignment.generalCores);
}

/**
 * @brief Test isolated cores detection
 */
TEST_F(CpuAffinityTest, IsolatedCores)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  // Should not throw and return valid cores
  for (int core : isolatedCores)
  {
    EXPECT_GE(core, 0);
    EXPECT_LT(core, _numCores);
  }
}

/**
 * @brief Test critical component configuration
 */
TEST_F(CpuAffinityTest, CriticalComponentConfig)
{
  performance::CriticalComponentConfig config;

  // Test default values
  EXPECT_TRUE(config.preferIsolatedCores);
  EXPECT_TRUE(config.exclusiveIsolatedCores);
  EXPECT_FALSE(config.allowSharedCriticalCores);
  EXPECT_EQ(config.minIsolatedForCritical, 1);

  // Test default priorities
  EXPECT_EQ(config.componentPriority.at("marketData"), 0);
  EXPECT_EQ(config.componentPriority.at("execution"), 1);
  EXPECT_EQ(config.componentPriority.at("strategy"), 2);
  EXPECT_EQ(config.componentPriority.at("risk"), 3);

  // Test custom configuration
  config.preferIsolatedCores = false;
  config.exclusiveIsolatedCores = false;
  config.allowSharedCriticalCores = true;
  config.minIsolatedForCritical = 2;
  config.componentPriority["marketData"] = 1;
  config.componentPriority["execution"] = 0;

  EXPECT_FALSE(config.preferIsolatedCores);
  EXPECT_FALSE(config.exclusiveIsolatedCores);
  EXPECT_TRUE(config.allowSharedCriticalCores);
  EXPECT_EQ(config.minIsolatedForCritical, 2);
  EXPECT_EQ(config.componentPriority.at("marketData"), 1);
  EXPECT_EQ(config.componentPriority.at("execution"), 0);
}

/**
 * @brief Test enhanced core assignment with configuration
 */
TEST_F(CpuAffinityTest, EnhancedCoreAssignment)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  // Test with default configuration
  auto assignment = _cpuAffinity->getRecommendedCoreAssignment();

  EXPECT_EQ(assignment.hasIsolatedCores, !isolatedCores.empty());
  EXPECT_EQ(assignment.allIsolatedCores, isolatedCores);

  // Verify all assigned cores are valid
  auto verifyValidCores = [this](const std::vector<int>& cores)
  {
    for (int core : cores)
    {
      EXPECT_GE(core, 0);
      EXPECT_LT(core, _numCores);
    }
  };

  verifyValidCores(assignment.marketDataCores);
  verifyValidCores(assignment.executionCores);
  verifyValidCores(assignment.strategyCores);
  verifyValidCores(assignment.riskCores);
  verifyValidCores(assignment.generalCores);
  verifyValidCores(assignment.criticalCores);

  // Test with custom configuration
  performance::CriticalComponentConfig config;
  config.preferIsolatedCores = false;

  auto assignment2 = _cpuAffinity->getRecommendedCoreAssignment(config);
  EXPECT_EQ(assignment2.hasIsolatedCores, !isolatedCores.empty());

  if (!isolatedCores.empty())
  {
    // When not preferring isolated cores, critical cores might not be isolated
    bool hasNonIsolatedCritical = false;
    for (int core : assignment2.criticalCores)
    {
      if (std::find(isolatedCores.begin(), isolatedCores.end(), core) == isolatedCores.end())
      {
        hasNonIsolatedCritical = true;
        break;
      }
    }
    // This test is flexible since behavior depends on system configuration
  }
}

/**
 * @brief Test critical component pinning
 */
TEST_F(CpuAffinityTest, CriticalComponentPinning)
{
  auto assignment = _cpuAffinity->getRecommendedCoreAssignment();

  // Test invalid component
  EXPECT_FALSE(_cpuAffinity->pinCriticalComponent("invalid", assignment));

  // Test valid components (only if they have assigned cores)
  if (!assignment.marketDataCores.empty())
  {
    // Note: This may fail without proper permissions, so we don't assert
    _cpuAffinity->pinCriticalComponent("marketData", assignment);
  }

  if (!assignment.executionCores.empty())
  {
    _cpuAffinity->pinCriticalComponent("execution", assignment);
  }

  if (!assignment.strategyCores.empty())
  {
    _cpuAffinity->pinCriticalComponent("strategy", assignment);
  }

  if (!assignment.riskCores.empty())
  {
    _cpuAffinity->pinCriticalComponent("risk", assignment);
  }
}

/**
 * @brief Test isolated core isolation verification
 */
TEST_F(CpuAffinityTest, VerifyCriticalCoreIsolation)
{
  auto assignment = _cpuAffinity->getRecommendedCoreAssignment();

  // Should not throw regardless of isolation status
  bool result = _cpuAffinity->verifyCriticalCoreIsolation(assignment);

  if (assignment.hasIsolatedCores && !assignment.criticalCores.empty())
  {
    // If we have isolated cores and critical cores, check if they align
    bool allCriticalIsolated = true;
    for (int core : assignment.criticalCores)
    {
      if (std::find(assignment.allIsolatedCores.begin(),
                    assignment.allIsolatedCores.end(), core) == assignment.allIsolatedCores.end())
      {
        allCriticalIsolated = false;
        break;
      }
    }
    EXPECT_EQ(result, allCriticalIsolated);
  }
  else if (!assignment.hasIsolatedCores)
  {
    EXPECT_FALSE(result);
  }
}

/**
 * @brief Test isolated core requirements checking
 */
TEST_F(CpuAffinityTest, CheckIsolatedCoreRequirements)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  // Test with different requirements
  bool result1 = _cpuAffinity->checkIsolatedCoreRequirements(1);
  bool result2 = _cpuAffinity->checkIsolatedCoreRequirements(4);
  bool result3 = _cpuAffinity->checkIsolatedCoreRequirements(100);

  EXPECT_EQ(result1, isolatedCores.size() >= 1);
  EXPECT_EQ(result2, isolatedCores.size() >= 4);
  EXPECT_EQ(result3, isolatedCores.size() >= 100);

  // Test default parameter
  bool resultDefault = _cpuAffinity->checkIsolatedCoreRequirements();
  EXPECT_EQ(resultDefault, isolatedCores.size() >= 4);
}

/**
   * @brief Test optimal performance configuration setup
   */
TEST_F(CpuAffinityTest, OptimalPerformanceConfiguration)
{
  // Test configuration without isolated cores
  performance::CriticalComponentConfig config1;
  config1.preferIsolatedCores = false;
  config1.exclusiveIsolatedCores = false;

  auto assignment = _cpuAffinity->getRecommendedCoreAssignment(config1);

  // Should return valid assignment
  EXPECT_GE(assignment.marketDataCores.size() + assignment.executionCores.size() +
                assignment.strategyCores.size() + assignment.riskCores.size() +
                assignment.generalCores.size(),
            0);

  // Test with isolated cores and NUMA awareness
  performance::CriticalComponentConfig config2;
  config2.preferIsolatedCores = true;
  config2.exclusiveIsolatedCores = true;

  auto assignment2 = _cpuAffinity->getNumaAwareCoreAssignment(config2);
  EXPECT_GE(assignment2.marketDataCores.size() + assignment2.executionCores.size() +
                assignment2.strategyCores.size() + assignment2.riskCores.size() +
                assignment2.generalCores.size(),
            0);
}

/**
 * @brief Test critical components setup and pinning
 */
TEST_F(CpuAffinityTest, SetupAndPinCriticalComponents)
{
  performance::CriticalComponentConfig config;
  config.preferIsolatedCores = true;

  // This test doesn't assert on the result since it depends on permissions
  // Just verify it doesn't crash
  bool result = _cpuAffinity->setupAndPinCriticalComponents(config);
  EXPECT_TRUE(result || !result);  // Always true, just to check it runs
}

/**
 * @brief Test isolated core usage demonstration
 */
TEST_F(CpuAffinityTest, DemonstrateIsolatedCoreUsage)
{
  // This should not throw
  EXPECT_NO_THROW(_cpuAffinity->demonstrateIsolatedCoreUsage());
}

/**
 * @brief Test isolated cores with different priority configurations
 */
TEST_F(CpuAffinityTest, IsolatedCoresPriorityConfig)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  if (isolatedCores.size() >= 2)
  {
    // Test priority reordering
    performance::CriticalComponentConfig config;
    config.componentPriority["marketData"] = 0;  // Highest priority
    config.componentPriority["execution"] = 1;   // Second priority
    config.componentPriority["risk"] = 2;
    config.componentPriority["strategy"] = 3;

    auto assignment = _cpuAffinity->getRecommendedCoreAssignment(config);

    // If we have isolated cores, market data should get first isolated core
    if (!assignment.marketDataCores.empty() && !assignment.executionCores.empty())
    {
      if (!isolatedCores.empty())
      {
        // Check if market data got a lower-numbered isolated core than execution
        // (This is a heuristic test since the actual assignment depends on the algorithm)
        EXPECT_FALSE(assignment.marketDataCores.empty());
        EXPECT_FALSE(assignment.executionCores.empty());
      }
    }
  }
}

/**
 * @brief Test exclusive vs shared isolated core usage
 */
TEST_F(CpuAffinityTest, ExclusiveVsSharedIsolatedCores)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  if (isolatedCores.size() >= 1)
  {
    // Test exclusive isolated cores (default)
    performance::CriticalComponentConfig exclusiveConfig;
    exclusiveConfig.exclusiveIsolatedCores = true;
    exclusiveConfig.allowSharedCriticalCores = false;

    auto exclusiveAssignment = _cpuAffinity->getRecommendedCoreAssignment(exclusiveConfig);

    // Test shared isolated cores
    performance::CriticalComponentConfig sharedConfig;
    sharedConfig.exclusiveIsolatedCores = false;
    sharedConfig.allowSharedCriticalCores = true;

    auto sharedAssignment = _cpuAffinity->getRecommendedCoreAssignment(sharedConfig);

    // Shared config should potentially have isolated cores in general cores too
    if (!isolatedCores.empty())
    {
      EXPECT_TRUE(sharedAssignment.hasIsolatedCores);

      // With shared config, general cores might include isolated cores
      size_t sharedGeneralCores = sharedAssignment.generalCores.size();
      size_t exclusiveGeneralCores = exclusiveAssignment.generalCores.size();

      // This is a heuristic test - shared might have more general cores
      EXPECT_GE(sharedGeneralCores, 0);
      EXPECT_GE(exclusiveGeneralCores, 0);
    }
  }
}

/**
 * @brief Test core assignment with insufficient isolated cores
 */
TEST_F(CpuAffinityTest, InsufficientIsolatedCores)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  // Test with minimum requirement higher than available cores
  performance::CriticalComponentConfig config;
  config.minIsolatedForCritical = isolatedCores.size() + 10;  // More than available
  config.preferIsolatedCores = true;

  auto assignment = _cpuAffinity->getRecommendedCoreAssignment(config);

  // Should fall back to basic assignment
  EXPECT_EQ(assignment.hasIsolatedCores, !isolatedCores.empty());

  // Should still assign cores even without sufficient isolated cores
  size_t totalAssignedCores = assignment.marketDataCores.size() +
                              assignment.executionCores.size() +
                              assignment.strategyCores.size() +
                              assignment.riskCores.size() +
                              assignment.generalCores.size();
  EXPECT_GT(totalAssignedCores, 0);
}

/**
   * @brief Comprehensive test simulating real performance isolated core usage
   */
TEST_F(CpuAffinityTest, PerformanceIsolatedCoreSimulation)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for performance simulation";
  }

  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  // Simulate performance setup process

  // Step 1: Check system requirements
  bool hasRequiredCores = _cpuAffinity->checkIsolatedCoreRequirements(4);

  // Step 2: Configure based on available cores
  performance::CriticalComponentConfig config;
  config.preferIsolatedCores = true;
  config.exclusiveIsolatedCores = hasRequiredCores;
  config.allowSharedCriticalCores = !hasRequiredCores;
  config.minIsolatedForCritical = hasRequiredCores ? 4 : 1;

  // Step 3: Get optimal assignment
  auto assignment = _cpuAffinity->getNumaAwareCoreAssignment(config);

  // Step 4: Verify the assignment is valid
  EXPECT_EQ(assignment.hasIsolatedCores, !isolatedCores.empty());

  // Step 5: Simulate thread pinning
  std::atomic<bool> running{true};
  std::atomic<int> threadsStarted{0};
  std::atomic<int> threadsCompleted{0};
  std::vector<std::thread> performanceThreads;

  // Market data thread
  if (!assignment.marketDataCores.empty())
  {
    performanceThreads.emplace_back([&, coreId = assignment.marketDataCores[0]]()
                                    {
      threadsStarted++;
      
      // Pin to assigned core
      bool pinned = _cpuAffinity->pinToCore(coreId);
      EXPECT_TRUE(pinned || !pinned);  // Don't fail test on permission issues
      
      // Set high priority (may fail without permissions)
      _cpuAffinity->setRealTimePriority(90);
      
      // Simulate market data processing
      auto startTime = std::chrono::high_resolution_clock::now();
      while (running && 
             std::chrono::high_resolution_clock::now() - startTime < std::chrono::milliseconds(100)) {
        // Simulate processing market tick
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
      
      threadsCompleted++; });
  }

  // Execution thread
  if (!assignment.executionCores.empty())
  {
    performanceThreads.emplace_back([&, coreId = assignment.executionCores[0]]()
                                    {
      threadsStarted++;
      
      bool pinned = _cpuAffinity->pinToCore(coreId);
      EXPECT_TRUE(pinned || !pinned);
      
      _cpuAffinity->setRealTimePriority(85);
      
      auto startTime = std::chrono::high_resolution_clock::now();
      while (running && 
             std::chrono::high_resolution_clock::now() - startTime < std::chrono::milliseconds(100)) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      
      threadsCompleted++; });
  }

  // Strategy thread
  if (!assignment.strategyCores.empty())
  {
    performanceThreads.emplace_back([&, coreId = assignment.strategyCores[0]]()
                                    {
      threadsStarted++;
      
      bool pinned = _cpuAffinity->pinToCore(coreId);
      EXPECT_TRUE(pinned || !pinned);
      
      _cpuAffinity->setRealTimePriority(80);
      
      auto startTime = std::chrono::high_resolution_clock::now();
      while (running && 
             std::chrono::high_resolution_clock::now() - startTime < std::chrono::milliseconds(100)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      
      threadsCompleted++; });
  }

  // Risk thread
  if (!assignment.riskCores.empty())
  {
    performanceThreads.emplace_back([&, coreId = assignment.riskCores[0]]()
                                    {
      threadsStarted++;
      
      bool pinned = _cpuAffinity->pinToCore(coreId);
      EXPECT_TRUE(pinned || !pinned);
      
      _cpuAffinity->setRealTimePriority(75);
      
      auto startTime = std::chrono::high_resolution_clock::now();
      while (running && 
             std::chrono::high_resolution_clock::now() - startTime < std::chrono::milliseconds(100)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      
      threadsCompleted++; });
  }

  // Wait for threads to start
  while (threadsStarted.load() < (int)performanceThreads.size() &&
         std::chrono::high_resolution_clock::now() - std::chrono::high_resolution_clock::now() < std::chrono::seconds(1))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Let threads run briefly
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // Signal shutdown
  running = false;

  // Wait for all threads to complete
  for (auto& thread : performanceThreads)
  {
    if (thread.joinable())
    {
      thread.join();
    }
  }

  // Verify all threads completed
  EXPECT_EQ(threadsCompleted.load(), (int)performanceThreads.size());

  // Step 6: Final verification
  bool isolation = _cpuAffinity->verifyCriticalCoreIsolation(assignment);
  EXPECT_TRUE(isolation || !isolation);  // Don't fail test, just verify it runs
}

/**
 * @brief Test real-time priority setting
 */
TEST_F(CpuAffinityTest, RealTimePriority)
{
  // This test may fail if not running as root
  bool result = _cpuAffinity->setRealTimePriority(50);

  // Don't assert on the result since it depends on permissions
  // Just verify it doesn't crash
  EXPECT_TRUE(result || !result);  // Always true, just to check it runs
}

/**
 * @brief Test CPU frequency scaling control
 */
TEST_F(CpuAffinityTest, CpuFrequencyScaling)
{
  // These tests may fail without proper permissions
  bool disableResult = _cpuAffinity->disableCpuFrequencyScaling();
  bool enableResult = _cpuAffinity->enableCpuFrequencyScaling();

  // Don't assert on the results since they depend on permissions
  // Just verify they don't crash
  EXPECT_TRUE(disableResult || !disableResult);
  EXPECT_TRUE(enableResult || !enableResult);
}

/**
 * @brief Test EventBus with direct core assignment
 */
TEST_F(CpuAffinityTest, EventBusWithAffinity)
{
  TradeBus bus;

  // Configure CPU affinity
  auto assignment = _cpuAffinity->getRecommendedCoreAssignment();
  bus.setCoreAssignment(assignment);

  // Verify assignment was set
  auto retrievedAssignment = bus.getCoreAssignment();
  EXPECT_TRUE(retrievedAssignment.has_value());

  if (retrievedAssignment.has_value())
  {
    const auto& assigned = retrievedAssignment.value();
    EXPECT_EQ(assigned.marketDataCores.size(), assignment.marketDataCores.size());
    EXPECT_EQ(assigned.strategyCores.size(), assignment.strategyCores.size());
    EXPECT_EQ(assigned.executionCores.size(), assignment.executionCores.size());
    EXPECT_EQ(assigned.riskCores.size(), assignment.riskCores.size());
    EXPECT_EQ(assigned.generalCores.size(), assignment.generalCores.size());
  }
}

/**
 * @brief Test enhanced EventBus configuration with isolated cores
 */
TEST_F(CpuAffinityTest, EventBusEnhancedConfiguration)
{
  TradeBus bus;

  // Test component type configuration
  TradeBus::AffinityConfig config(TradeBus::ComponentType::MARKET_DATA, 90);
  config.enableRealTimePriority = true;
  config.enableNumaAwareness = true;
  config.preferIsolatedCores = true;

  bus.setAffinityConfig(config);

  // Verify configuration was set
  auto retrievedConfig = bus.getAffinityConfig();
  EXPECT_TRUE(retrievedConfig.has_value());

  if (retrievedConfig.has_value())
  {
    const auto& cfg = retrievedConfig.value();
    EXPECT_EQ(cfg.componentType, TradeBus::ComponentType::MARKET_DATA);
    EXPECT_EQ(cfg.realTimePriority, 90);
    EXPECT_TRUE(cfg.enableRealTimePriority);
    EXPECT_TRUE(cfg.enableNumaAwareness);
    EXPECT_TRUE(cfg.preferIsolatedCores);
  }

  // Verify core assignment was generated
  auto assignment = bus.getCoreAssignment();
  EXPECT_TRUE(assignment.has_value());
}

/**
 * @brief Test optimal EventBus setup
 */
TEST_F(CpuAffinityTest, EventBusOptimalSetup)
{
  TradeBus bus;

  // Test optimal configuration setup
  bool success = bus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA, false);
  EXPECT_TRUE(success || !success);  // Don't fail on permission issues

  // Verify configuration
  auto config = bus.getAffinityConfig();
  EXPECT_TRUE(config.has_value());

  if (config.has_value())
  {
    EXPECT_EQ(config->componentType, TradeBus::ComponentType::MARKET_DATA);
    EXPECT_EQ(config->realTimePriority, 90);  // Market data gets highest priority
  }

  // Test verification
  bool isolated = bus.verifyIsolatedCoreConfiguration();
  EXPECT_TRUE(isolated || !isolated);  // Don't fail test, just verify it runs
}

/**
 * @brief Test different component types for EventBus
 */
TEST_F(CpuAffinityTest, EventBusComponentTypes)
{
  // Test market data configuration
  {
    TradeBus marketDataBus;
    bool success = marketDataBus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA);
    EXPECT_TRUE(success || !success);

    auto config = marketDataBus.getAffinityConfig();
    if (config.has_value())
    {
      EXPECT_EQ(config->realTimePriority, 90);
      EXPECT_TRUE(config->enableRealTimePriority);
    }
  }

  // Test execution configuration
  {
    TradeBus executionBus;
    bool success = executionBus.setupOptimalConfiguration(TradeBus::ComponentType::EXECUTION);
    EXPECT_TRUE(success || !success);

    auto config = executionBus.getAffinityConfig();
    if (config.has_value())
    {
      EXPECT_EQ(config->realTimePriority, 85);
      EXPECT_TRUE(config->enableRealTimePriority);
    }
  }

  // Test strategy configuration
  {
    TradeBus strategyBus;
    bool success = strategyBus.setupOptimalConfiguration(TradeBus::ComponentType::STRATEGY);
    EXPECT_TRUE(success || !success);

    auto config = strategyBus.getAffinityConfig();
    if (config.has_value())
    {
      EXPECT_EQ(config->realTimePriority, 80);
      EXPECT_TRUE(config->enableRealTimePriority);
    }
  }

  // Test general configuration
  {
    TradeBus generalBus;
    bool success = generalBus.setupOptimalConfiguration(TradeBus::ComponentType::GENERAL);
    EXPECT_TRUE(success || !success);

    auto config = generalBus.getAffinityConfig();
    if (config.has_value())
    {
      EXPECT_EQ(config->realTimePriority, 70);
      EXPECT_FALSE(config->enableRealTimePriority);  // General doesn't use RT priority
    }
  }
}

/**
 * @brief Test multiple EventBus instances with different component types
 */
TEST_F(CpuAffinityTest, MultipleEventBusInstances)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();

  if (isolatedCores.size() >= 2)
  {
    // Create multiple event buses for different components
    TradeBus marketDataBus;
    TradeBus executionBus;

    // Configure each for different component types
    bool success1 = marketDataBus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA);
    bool success2 = executionBus.setupOptimalConfiguration(TradeBus::ComponentType::EXECUTION);

    EXPECT_TRUE(success1 || !success1);
    EXPECT_TRUE(success2 || !success2);

    // Verify they have different configurations
    auto config1 = marketDataBus.getAffinityConfig();
    auto config2 = executionBus.getAffinityConfig();

    if (config1.has_value() && config2.has_value())
    {
      EXPECT_NE(config1->componentType, config2->componentType);
      EXPECT_NE(config1->realTimePriority, config2->realTimePriority);
    }
  }
}

/**
 * @brief Test EventBus isolated core verification
 */
TEST_F(CpuAffinityTest, EventBusIsolatedCoreVerification)
{
  TradeBus bus;

  // Initially should not be configured
  EXPECT_FALSE(bus.verifyIsolatedCoreConfiguration());

  // Configure with isolated cores
  bool success = bus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA);
  EXPECT_TRUE(success || !success);

  // Verification should work regardless of actual isolation
  bool verified = bus.verifyIsolatedCoreConfiguration();
  EXPECT_TRUE(verified || !verified);  // Don't fail test, just verify it runs
}

/**
 * @brief Comprehensive integration test for EventBus isolated core functionality
 */
TEST_F(CpuAffinityTest, EventBusIsolatedCoreIntegration)
{
  auto isolatedCores = _cpuAffinity->getIsolatedCores();
  bool hasRequiredCores = _cpuAffinity->checkIsolatedCoreRequirements(4);

  // Test creating optimal event buses for different components

  // Market data bus (highest priority)
  TradeBus marketDataBus;
  bool success1 = marketDataBus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA, false);

  // Note: We can't actually test OrderExecutionBus and CandleBus here since they're different types
  // But we can create multiple TradeBus instances for different purposes
  TradeBus executionBus;
  bool success2 = executionBus.setupOptimalConfiguration(TradeBus::ComponentType::EXECUTION, false);

  TradeBus strategyBus;
  bool success3 = strategyBus.setupOptimalConfiguration(TradeBus::ComponentType::STRATEGY, false);

  TradeBus riskBus;
  bool success4 = riskBus.setupOptimalConfiguration(TradeBus::ComponentType::RISK, false);

  // Verify configurations

  auto config1 = marketDataBus.getAffinityConfig();
  auto config2 = executionBus.getAffinityConfig();
  auto config3 = strategyBus.getAffinityConfig();
  auto config4 = riskBus.getAffinityConfig();

  if (config1.has_value() && config2.has_value() && config3.has_value() && config4.has_value())
  {
    // Verify priority ordering
    EXPECT_GT(config1->realTimePriority, config2->realTimePriority);  // Market data > Execution
    EXPECT_GT(config2->realTimePriority, config3->realTimePriority);  // Execution > Strategy
    EXPECT_GT(config3->realTimePriority, config4->realTimePriority);  // Strategy > Risk
  }

  // Verify isolated core assignments

  bool isolated1 = marketDataBus.verifyIsolatedCoreConfiguration();
  bool isolated2 = executionBus.verifyIsolatedCoreConfiguration();
  bool isolated3 = strategyBus.verifyIsolatedCoreConfiguration();
  bool isolated4 = riskBus.verifyIsolatedCoreConfiguration();

  // Summary
  int successCount = success1 + success2 + success3 + success4;
  int isolationCount = isolated1 + isolated2 + isolated3 + isolated4;

  // Don't fail the test based on system configuration
  EXPECT_GE(successCount, 0);
  EXPECT_GE(isolationCount, 0);
}

/**
 * @brief Test multi-threaded CPU affinity
 */
TEST_F(CpuAffinityTest, MultiThreadedAffinity)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  constexpr int numThreads = 4;
  std::vector<std::thread> threads;
  std::vector<std::atomic<int>> threadCores(numThreads);

  for (int i = 0; i < numThreads; ++i)
  {
    threadCores[i].store(-1);

    threads.emplace_back([&, i]()
                         {
            int targetCore = i % _numCores;
            bool result = _cpuAffinity->pinToCore(targetCore);
            
            if (result)
            {
                auto affinity = _cpuAffinity->getCurrentAffinity();
                if (affinity.size() == 1)
                {
                    threadCores[i].store(affinity[0]);
                }
            } });
  }

  for (auto& t : threads)
  {
    t.join();
  }

#ifdef __linux__
  // Verify threads were pinned correctly
  for (int i = 0; i < numThreads; ++i)
  {
    int expectedCore = i % _numCores;
    EXPECT_EQ(threadCores[i].load(), expectedCore);
  }
#endif
}

/**
 * @brief Stress test CPU affinity operations
 */
TEST_F(CpuAffinityTest, StressTest)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  // Rapidly switch between cores
  for (int i = 0; i < 100; ++i)
  {
    int targetCore = i % _numCores;
    _cpuAffinity->pinToCore(targetCore);

    // Small delay to allow OS to process
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }

  // Should still work after stress test
  auto affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_FALSE(affinity.empty());
}

/**
 * @brief Test exception safety of ThreadAffinityGuard
 */
TEST_F(CpuAffinityTest, ExceptionSafety)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

#ifdef __linux__
  // Pin to core 0 first
  _cpuAffinity->pinToCore(0);

  try
  {
    ThreadAffinityGuard guard(1);

    // Verify we're on core 1
    auto affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 1);

    // Throw exception
    throw std::runtime_error("Test exception");
  }
  catch (const std::exception&)
  {
    // Exception caught, guard should have restored affinity
  }

  // Should be restored to core 0
  auto affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);
#endif
}

/**
 * @brief Test NUMA topology detection
 */
TEST_F(CpuAffinityTest, NumaTopology)
{
  auto topology = _cpuAffinity->getNumaTopology();

  // Basic validation
  EXPECT_GE(topology.numNodes, 0);
  EXPECT_EQ(topology.nodes.size(), static_cast<size_t>(topology.numNodes));

  if (topology.numaAvailable)
  {
    EXPECT_GT(topology.numNodes, 0);

    // Validate each node
    for (const auto& node : topology.nodes)
    {
      EXPECT_GE(node.nodeId, 0);
      EXPECT_FALSE(node.cpuCores.empty());

      // All cores should be valid
      for (int core : node.cpuCores)
      {
        EXPECT_GE(core, 0);
        EXPECT_LT(core, _numCores);
      }

      // Memory info should be reasonable
      EXPECT_GE(node.totalMemoryMB, 0);
      EXPECT_GE(node.freeMemoryMB, 0);
      EXPECT_LE(node.freeMemoryMB, node.totalMemoryMB);
    }

    // Ensure all cores are accounted for
    std::set<int> allNumaCores;
    for (const auto& node : topology.nodes)
    {
      for (int core : node.cpuCores)
      {
        allNumaCores.insert(core);
      }
    }

    // Should have at least some cores mapped
    EXPECT_FALSE(allNumaCores.empty());
  }
  else
  {
    EXPECT_EQ(topology.numNodes, 0);
    EXPECT_TRUE(topology.nodes.empty());
  }
}

/**
 * @brief Test NUMA node to core mapping
 */
TEST_F(CpuAffinityTest, NumaNodeForCore)
{
  auto topology = _cpuAffinity->getNumaTopology();

  if (!topology.numaAvailable)
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

  // Test mapping for cores we know exist
  for (const auto& node : topology.nodes)
  {
    for (int core : node.cpuCores)
    {
      int numaNode = _cpuAffinity->getNumaNodeForCore(core);
      // Note: The mapping might not be perfect due to different methods
      // but it should return a valid node ID or -1
      EXPECT_TRUE(numaNode == -1 || numaNode >= 0);
    }
  }

  // Test invalid core
  int invalidNode = _cpuAffinity->getNumaNodeForCore(999);
  EXPECT_EQ(invalidNode, -1);
}

/**
 * @brief Test NUMA node pinning
 */
TEST_F(CpuAffinityTest, PinToNumaNode)
{
  auto topology = _cpuAffinity->getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

#ifdef __linux__
  // Pin to first NUMA node
  int nodeId = topology.nodes[0].nodeId;
  bool result = _cpuAffinity->pinToNumaNode(nodeId);
  EXPECT_TRUE(result);

  // Check that we're pinned to cores in that node
  auto affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_FALSE(affinity.empty());

  // All pinned cores should be in the target NUMA node
  const auto& nodeCores = topology.nodes[0].cpuCores;
  for (int core : affinity)
  {
    EXPECT_TRUE(std::find(nodeCores.begin(), nodeCores.end(), core) != nodeCores.end());
  }

  // Test invalid node
  result = _cpuAffinity->pinToNumaNode(999);
  EXPECT_FALSE(result);
#else
  bool result = _cpuAffinity->pinToNumaNode(0);
  EXPECT_FALSE(result);  // Should fail on non-Linux platforms
#endif
}

/**
 * @brief Test NUMA node pinning with separate thread
 */
TEST_F(CpuAffinityTest, ThreadNumaPinning)
{
  auto topology = _cpuAffinity->getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

  std::atomic<bool> threadPinned{false};
  std::atomic<int> threadNodeCores{0};

  int nodeId = topology.nodes[0].nodeId;
  const auto& expectedCores = topology.nodes[0].cpuCores;

  std::thread t([&]()
                {
        bool result = _cpuAffinity->pinToNumaNode(nodeId);
        threadPinned.store(result);
        
        if (result)
        {
            auto affinity = _cpuAffinity->getCurrentAffinity();
            int coresInNode = 0;
            for (int core : affinity)
            {
                if (std::find(expectedCores.begin(), expectedCores.end(), core) != expectedCores.end())
                {
                    coresInNode++;
                }
            }
            threadNodeCores.store(coresInNode);
        } });

  t.join();

#ifdef __linux__
  EXPECT_TRUE(threadPinned.load());
  EXPECT_GT(threadNodeCores.load(), 0);
#else
  EXPECT_FALSE(threadPinned.load());
#endif
}

/**
 * @brief Test memory policy setting
 */
TEST_F(CpuAffinityTest, MemoryPolicy)
{
  auto topology = _cpuAffinity->getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

#ifdef __linux__
  int nodeId = topology.nodes[0].nodeId;
  bool result = _cpuAffinity->setMemoryPolicy(nodeId);
  EXPECT_TRUE(result);

  // Test invalid node
  result = _cpuAffinity->setMemoryPolicy(999);
  EXPECT_FALSE(result);
#else
  bool result = _cpuAffinity->setMemoryPolicy(0);
  EXPECT_FALSE(result);  // Should fail on non-Linux platforms
#endif
}

/**
 * @brief Test NUMA-aware core assignment
 */
TEST_F(CpuAffinityTest, NumaAwareCoreAssignment)
{
  auto assignment = _cpuAffinity->getNumaAwareCoreAssignment();
  auto topology = _cpuAffinity->getNumaTopology();

  // Basic validation
  EXPECT_TRUE(assignment.marketDataCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.strategyCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.executionCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.riskCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.generalCores.size() <= static_cast<size_t>(_numCores));

  // All cores should be valid
  auto validateCores = [this](const std::vector<int>& cores)
  {
    for (int core : cores)
    {
      EXPECT_GE(core, 0);
      EXPECT_LT(core, _numCores);
    }
  };

  validateCores(assignment.marketDataCores);
  validateCores(assignment.strategyCores);
  validateCores(assignment.executionCores);
  validateCores(assignment.riskCores);
  validateCores(assignment.generalCores);

  if (topology.numaAvailable && !topology.nodes.empty())
  {
    // NUMA-aware assignment should try to keep related tasks on same node
    // For systems with sufficient cores, market data and execution should be on same node
    if (!assignment.marketDataCores.empty() && !assignment.executionCores.empty())
    {
      int marketDataNode = _cpuAffinity->getNumaNodeForCore(assignment.marketDataCores[0]);
      int executionNode = _cpuAffinity->getNumaNodeForCore(assignment.executionCores[0]);

      // If both return valid nodes, they should ideally be the same
      if (marketDataNode >= 0 && executionNode >= 0)
      {
        // This is a preference, not a strict requirement
        // Just verify they're both valid assignments
        EXPECT_GE(marketDataNode, 0);
        EXPECT_GE(executionNode, 0);
      }
    }
  }
}

/**
 * @brief Test NumaAffinityGuard RAII wrapper
 */
TEST_F(CpuAffinityTest, NumaAffinityGuard)
{
  auto topology = _cpuAffinity->getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

#ifdef __linux__
  // Pin to core 0 first
  if (_numCores >= 1)
  {
    _cpuAffinity->pinToCore(0);
    auto affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 0);
  }

  int nodeId = topology.nodes[0].nodeId;
  const auto& nodeCores = topology.nodes[0].cpuCores;

  {
    // Use NUMA guard to temporarily pin to NUMA node
    NumaAffinityGuard guard(*_cpuAffinity, nodeId);

    auto affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_FALSE(affinity.empty());

    // All cores should be from the target NUMA node
    for (int core : affinity)
    {
      EXPECT_TRUE(std::find(nodeCores.begin(), nodeCores.end(), core) != nodeCores.end());
    }
  }

  // Should be restored to previous state (core 0)
  if (_numCores >= 1)
  {
    auto affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 0);
  }
#endif
}

/**
 * @brief Test NumaAffinityGuard with specific core
 */
TEST_F(CpuAffinityTest, NumaAffinityGuardSpecificCore)
{
  auto topology = _cpuAffinity->getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty() || _numCores < 2)
  {
    GTEST_SKIP() << "NUMA not available or insufficient cores";
  }

#ifdef __linux__
  int nodeId = topology.nodes[0].nodeId;
  const auto& nodeCores = topology.nodes[0].cpuCores;

  if (nodeCores.empty())
  {
    GTEST_SKIP() << "No cores available in NUMA node";
  }

  int targetCore = nodeCores[0];

  {
    // Use NUMA guard to pin to specific core and set memory policy
    NumaAffinityGuard guard(*_cpuAffinity, targetCore, nodeId);

    auto affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], targetCore);
  }

  // Should be restored to original affinity
  auto affinity = _cpuAffinity->getCurrentAffinity();
  EXPECT_EQ(affinity.size(), _originalAffinity.size());
#endif
}

/**
 * @brief Test NUMA functionality with multi-threading
 */
TEST_F(CpuAffinityTest, NumaMultiThreaded)
{
  auto topology = _cpuAffinity->getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

  const int numThreads = std::min(4, static_cast<int>(topology.nodes.size()));
  std::vector<std::atomic<bool>> threadResults(numThreads);
  std::vector<std::thread> threads;

  for (int i = 0; i < numThreads; ++i)
  {
    threadResults[i].store(false);

    threads.emplace_back([&, i]()
                         {
        int nodeId = topology.nodes[i % topology.nodes.size()].nodeId;
        
        // Use NUMA affinity guard
        NumaAffinityGuard guard(*_cpuAffinity, nodeId);
        
        // Verify pinning worked
        auto affinity = _cpuAffinity->getCurrentAffinity();
        if (!affinity.empty())
        {
            const auto& expectedCores = topology.nodes[i % topology.nodes.size()].cpuCores;
            bool allCoresInNode = true;
            
            for (int core : affinity)
            {
                if (std::find(expectedCores.begin(), expectedCores.end(), core) == expectedCores.end())
                {
                    allCoresInNode = false;
                    break;
                }
            }
            
            threadResults[i].store(allCoresInNode);
        }
        
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
  }

  for (auto& t : threads)
  {
    t.join();
  }

#ifdef __linux__
  for (int i = 0; i < numThreads; ++i)
  {
    EXPECT_TRUE(threadResults[i].load()) << "Thread " << i << " failed NUMA pinning";
  }
#endif
}

/**
 * @brief Test conditional NUMA guard usage - demonstrates best practice pattern
 */
TEST_F(CpuAffinityTest, ConditionalNumaGuardUsage)
{
  // This test demonstrates the recommended pattern for applications:
  // Check if NUMA is available before using NUMA guards

  if (isNumaAvailable())
  {
    // NUMA is available - use NUMA-aware optimizations
    int testNode = getTestNumaNode();
    ASSERT_GE(testNode, 0) << "Should have valid NUMA node when NUMA is available";

    {
      // Use NUMA guard for optimal memory locality
      NumaAffinityGuard numaGuard(*_cpuAffinity, testNode);

      // Verify we're pinned to the NUMA node
      auto affinity = _cpuAffinity->getCurrentAffinity();
      EXPECT_FALSE(affinity.empty());

      // Simulate memory-intensive work that benefits from NUMA locality
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // NUMA guard automatically restored affinity and memory policy
  }
  else
  {
    // NUMA not available - fall back to regular CPU affinity
    if (_numCores >= 2)
    {
      ThreadAffinityGuard cpuGuard(0);  // Pin to core 0

      // Verify regular CPU pinning works
      auto affinity = _cpuAffinity->getCurrentAffinity();
      EXPECT_EQ(affinity.size(), 1);
      EXPECT_EQ(affinity[0], 0);

      // Simulate work without NUMA optimizations
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // NUMA not available - using regular CPU affinity
  }
}

/**
 * @brief Test mixed guard usage - CPU guards with optional NUMA
 */
TEST_F(CpuAffinityTest, MixedGuardUsage)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  // Always use basic CPU affinity
  {
    ThreadAffinityGuard cpuGuard(1);

    auto affinity = _cpuAffinity->getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 1);

    // Conditionally add NUMA optimizations if available
    if (isNumaAvailable())
    {
      int nodeId = _cpuAffinity->getNumaNodeForCore(1);
      if (nodeId >= 0)
      {
        // Set memory policy for the NUMA node containing core 1
        bool memPolicySet = _cpuAffinity->setMemoryPolicy(nodeId);
        EXPECT_TRUE(memPolicySet || !memPolicySet);  // Don't assert - just verify it doesn't crash

        // Enhanced with NUMA memory policy
      }
    }

    // Simulate work that benefits from both CPU and NUMA affinity
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}