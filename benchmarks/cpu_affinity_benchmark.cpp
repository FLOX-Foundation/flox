/*
 * Flox Engine - CPU Affinity Benchmarks
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * IMPORTANT: These benchmarks should be run on an isolated machine with minimal
 * background processes. CPU affinity can actually decrease performance on busy
 * or shared systems because:
 * - It prevents the OS scheduler from optimally distributing load across cores
 * - Pinned threads may compete with other processes on the same cores
 * - The OS loses flexibility to move threads to less busy cores
 * - System-wide performance can degrade due to poor load balancing
 *
 * For production systems, consider using CPU affinity only when you have
 * dedicated hardware and can control the entire system's workload.
 */

#include "flox/util/performance/cpu_affinity.h"

#include <benchmark/benchmark.h>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

using namespace flox::performance;

/**
 * @brief Benchmark basic CPU affinity operations
 */
static void BM_CpuAffinity_PinToCore(benchmark::State& state)
{
  int coreId = state.range(0);
  auto cpuAffinity = createCpuAffinity();

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(cpuAffinity->pinToCore(coreId));
  }
}
BENCHMARK(BM_CpuAffinity_PinToCore)->Range(0, 7);

/**
 * @brief Benchmark thread affinity guard overhead
 */
static void BM_CpuAffinity_ThreadAffinityGuard(benchmark::State& state)
{
  int coreId = state.range(0);

  for (auto _ : state)
  {
    ThreadAffinityGuard guard(coreId);
    benchmark::DoNotOptimize(guard);
  }
}
BENCHMARK(BM_CpuAffinity_ThreadAffinityGuard)->Range(0, 3);

/**
 * @brief Benchmark CPU topology queries
 */
static void BM_CpuAffinity_GetCurrentAffinity(benchmark::State& state)
{
  auto cpuAffinity = createCpuAffinity();

  for (auto _ : state)
  {
    auto affinity = cpuAffinity->getCurrentAffinity();
    benchmark::DoNotOptimize(affinity);
  }
}
BENCHMARK(BM_CpuAffinity_GetCurrentAffinity);

/**
 * @brief Memory access pattern benchmark with CPU affinity
 */
static void BM_MemoryAccess_WithAffinity(benchmark::State& state)
{
  const size_t arraySize = 1 << 20;  // 1MB array
  std::vector<int> data(arraySize);

  // Fill with random data
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(1, 1000);
  for (size_t i = 0; i < arraySize; ++i)
  {
    data[i] = dist(rng);
  }

  // Pin to specific core
  ThreadAffinityGuard guard(0);

  for (auto _ : state)
  {
    volatile int sum = 0;
    for (size_t i = 0; i < arraySize; ++i)
    {
      sum += data[i];
    }
    benchmark::DoNotOptimize(sum);
  }

  state.SetBytesProcessed(state.iterations() * arraySize * sizeof(int));
}
BENCHMARK(BM_MemoryAccess_WithAffinity);

/**
 * @brief Memory access pattern benchmark without CPU affinity
 */
static void BM_MemoryAccess_WithoutAffinity(benchmark::State& state)
{
  const size_t arraySize = 1 << 20;  // 1MB array
  std::vector<int> data(arraySize);

  // Fill with random data
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(1, 1000);
  for (size_t i = 0; i < arraySize; ++i)
  {
    data[i] = dist(rng);
  }

  // No CPU affinity - let OS schedule freely

  for (auto _ : state)
  {
    volatile int sum = 0;
    for (size_t i = 0; i < arraySize; ++i)
    {
      sum += data[i];
    }
    benchmark::DoNotOptimize(sum);
  }

  state.SetBytesProcessed(state.iterations() * arraySize * sizeof(int));
}
BENCHMARK(BM_MemoryAccess_WithoutAffinity);

/**
 * @brief Benchmark high-frequency trading simulation with CPU affinity
 * Simulates order processing workload pinned to specific cores
 */
static void BM_HighPerformance_OrderProcessing_WithAffinity(benchmark::State& state)
{
  ThreadAffinityGuard guard(0);  // Pin to core 0

  // Simulate order book operations
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> priceDist(100.0, 110.0);
  std::uniform_int_distribution<int> sizeDist(1, 100);

  uint64_t processedOrders = 0;
  double totalValue = 0.0;

  for (auto _ : state)
  {
    // Simulate order processing
    for (int i = 0; i < 1000; ++i)
    {
      double price = priceDist(rng);
      int size = sizeDist(rng);

      // Simulate order validation and matching
      double orderValue = price * size;
      totalValue += orderValue;

      // Simulate position updates
      bool isValid = (price > 99.0 && price < 111.0);
      if (isValid)
      {
        processedOrders++;
      }

      benchmark::DoNotOptimize(orderValue);
      benchmark::DoNotOptimize(isValid);
      benchmark::DoNotOptimize(processedOrders);
      benchmark::DoNotOptimize(totalValue);
    }
  }

  state.counters["ProcessedOrders"] = processedOrders;
  state.counters["TotalValue"] = totalValue;
}
BENCHMARK(BM_HighPerformance_OrderProcessing_WithAffinity);

/**
 * @brief Benchmark high-frequency trading simulation without CPU affinity
 * Same workload but without thread pinning for comparison
 */
static void BM_HighPerformance_OrderProcessing_WithoutAffinity(benchmark::State& state)
{
  // No CPU affinity

  // Simulate order book operations
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> priceDist(100.0, 110.0);
  std::uniform_int_distribution<int> sizeDist(1, 100);

  uint64_t processedOrders = 0;
  double totalValue = 0.0;

  for (auto _ : state)
  {
    // Simulate order processing
    for (int i = 0; i < 1000; ++i)
    {
      double price = priceDist(rng);
      int size = sizeDist(rng);

      // Simulate order validation and matching
      double orderValue = price * size;
      totalValue += orderValue;

      // Simulate position updates
      bool isValid = (price > 99.0 && price < 111.0);
      if (isValid)
      {
        processedOrders++;
      }

      benchmark::DoNotOptimize(orderValue);
      benchmark::DoNotOptimize(isValid);
      benchmark::DoNotOptimize(processedOrders);
      benchmark::DoNotOptimize(totalValue);
    }
  }

  state.counters["ProcessedOrders"] = processedOrders;
  state.counters["TotalValue"] = totalValue;
}
BENCHMARK(BM_HighPerformance_OrderProcessing_WithoutAffinity);

/**
 * @brief Benchmark context switching overhead
 */
static void BM_ContextSwitching_WithAffinity(benchmark::State& state)
{
  ThreadAffinityGuard guard(0);

  for (auto _ : state)
  {
    std::this_thread::yield();
  }
}
BENCHMARK(BM_ContextSwitching_WithAffinity);

/**
 * @brief Benchmark context switching overhead without affinity
 */
static void BM_ContextSwitching_WithoutAffinity(benchmark::State& state)
{
  for (auto _ : state)
  {
    std::this_thread::yield();
  }
}
BENCHMARK(BM_ContextSwitching_WithoutAffinity);

/**
 * @brief Benchmark computational workload with CPU affinity
 */
static void BM_Computation_WithAffinity(benchmark::State& state)
{
  ThreadAffinityGuard guard(0);

  for (auto _ : state)
  {
    // Simulate order book calculation
    volatile double price = 100.0;
    volatile double quantity = 1000.0;

    for (int i = 0; i < 1000; ++i)
    {
      price = price * 1.001 + quantity * 0.0001;
      quantity = quantity * 0.999 + price * 0.0001;
    }

    benchmark::DoNotOptimize(price);
    benchmark::DoNotOptimize(quantity);
  }
}
BENCHMARK(BM_Computation_WithAffinity);

/**
 * @brief Benchmark computational workload without CPU affinity
 */
static void BM_Computation_WithoutAffinity(benchmark::State& state)
{
  for (auto _ : state)
  {
    // Simulate order book calculation
    volatile double price = 100.0;
    volatile double quantity = 1000.0;

    for (int i = 0; i < 1000; ++i)
    {
      price = price * 1.001 + quantity * 0.0001;
      quantity = quantity * 0.999 + price * 0.0001;
    }

    benchmark::DoNotOptimize(price);
    benchmark::DoNotOptimize(quantity);
  }
}
BENCHMARK(BM_Computation_WithoutAffinity);

/**
 * @brief Benchmark multi-threaded workload with CPU affinity
 */
static void BM_MultiThreaded_WithAffinity(benchmark::State& state)
{
  const int numThreads = state.range(0);
  std::atomic<int> counter{0};
  auto cpuAffinity = createCpuAffinity();
  const auto numCores = cpuAffinity->getNumCores();

  for (auto _ : state)
  {
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int i = 0; i < numThreads; ++i)
    {
      threads.emplace_back([&counter, i, numCores]()
                           {
                auto threadCpuAffinity = createCpuAffinity();
                threadCpuAffinity->pinToCore(i % numCores);
                
                for (int j = 0; j < 1000; ++j)
                {
                    counter.fetch_add(1, std::memory_order_relaxed);
                } });
    }

    for (auto& t : threads)
    {
      t.join();
    }
  }

  state.counters["FinalCounter"] = counter.load();
}
BENCHMARK(BM_MultiThreaded_WithAffinity)->Range(2, 8);

/**
 * @brief Benchmark multi-threaded workload without CPU affinity
 */
static void BM_MultiThreaded_WithoutAffinity(benchmark::State& state)
{
  const int numThreads = state.range(0);
  std::atomic<int> counter{0};

  for (auto _ : state)
  {
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int i = 0; i < numThreads; ++i)
    {
      threads.emplace_back([&counter]()
                           {
                for (int j = 0; j < 1000; ++j)
                {
                    counter.fetch_add(1, std::memory_order_relaxed);
                } });
    }

    for (auto& t : threads)
    {
      t.join();
    }
  }

  state.counters["FinalCounter"] = counter.load();
}
BENCHMARK(BM_MultiThreaded_WithoutAffinity)->Range(2, 8);

BENCHMARK_MAIN();