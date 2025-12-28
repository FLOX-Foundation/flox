/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/mmap_bar_storage.h"

#include <benchmark/benchmark.h>
#include <filesystem>
#include <fstream>
#include <random>

using namespace flox;

namespace
{

std::filesystem::path g_test_dir;
bool g_initialized = false;

Bar makeBar(double price, int64_t endTimeMs)
{
  Bar bar;
  bar.open = Price::fromDouble(price);
  bar.high = Price::fromDouble(price + 1.0);
  bar.low = Price::fromDouble(price - 1.0);
  bar.close = Price::fromDouble(price);
  bar.volume = Volume::fromDouble(100.0);
  bar.endTime = TimePoint(std::chrono::milliseconds(endTimeMs));
  return bar;
}

void initTestData()
{
  if (g_initialized)
  {
    return;
  }

  g_test_dir = std::filesystem::temp_directory_path() / "flox_mmap_bench";
  std::filesystem::remove_all(g_test_dir);
  std::filesystem::create_directories(g_test_dir);

  // Create test files with various sizes
  std::vector<size_t> sizes = {1000, 10000, 100000, 1000000};

  for (size_t numBars : sizes)
  {
    std::string filename = "bars_" + std::to_string(numBars) + "s.bin";
    std::ofstream file(g_test_dir / filename, std::ios::binary);

    uint64_t count = numBars;
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (size_t i = 0; i < numBars; ++i)
    {
      Bar bar = makeBar(100.0 + i * 0.01, static_cast<int64_t>(i) * 60000);
      file.write(reinterpret_cast<const char*>(&bar), sizeof(bar));
    }
  }

  g_initialized = true;
}

void cleanupTestData()
{
  if (g_initialized)
  {
    std::filesystem::remove_all(g_test_dir);
    g_initialized = false;
  }
}

}  // namespace

// =============================================================================
// MmapBarStorage benchmarks
// =============================================================================

static void BM_MmapBarStorage_Open(benchmark::State& state)
{
  initTestData();

  for (auto _ : state)
  {
    MmapBarStorage storage(g_test_dir);
    benchmark::DoNotOptimize(storage.totalBars());
  }
}
BENCHMARK(BM_MmapBarStorage_Open);

static void BM_MmapBarStorage_GetBarSequential(benchmark::State& state)
{
  initTestData();
  MmapBarStorage storage(g_test_dir);

  auto tf = TimeframeId::time(std::chrono::seconds(100000));  // 100000 bars file
  size_t barCount = storage.barCount(tf);

  size_t idx = 0;
  for (auto _ : state)
  {
    const Bar* bar = storage.getBar(tf, idx);
    benchmark::DoNotOptimize(bar);
    idx = (idx + 1) % barCount;
  }
}
BENCHMARK(BM_MmapBarStorage_GetBarSequential);

static void BM_MmapBarStorage_GetBarRandom(benchmark::State& state)
{
  initTestData();
  MmapBarStorage storage(g_test_dir);

  auto tf = TimeframeId::time(std::chrono::seconds(100000));  // 100000 bars file
  size_t barCount = storage.barCount(tf);

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, barCount - 1);

  for (auto _ : state)
  {
    size_t idx = dist(rng);
    const Bar* bar = storage.getBar(tf, idx);
    benchmark::DoNotOptimize(bar);
  }
}
BENCHMARK(BM_MmapBarStorage_GetBarRandom);

static void BM_MmapBarStorage_GetBarsSpan(benchmark::State& state)
{
  initTestData();
  MmapBarStorage storage(g_test_dir);

  auto tf = TimeframeId::time(std::chrono::seconds(100000));

  for (auto _ : state)
  {
    auto span = storage.getBars(tf);
    benchmark::DoNotOptimize(span.data());
    benchmark::DoNotOptimize(span.size());
  }
}
BENCHMARK(BM_MmapBarStorage_GetBarsSpan);

static void BM_MmapBarStorage_IterateAllBars(benchmark::State& state)
{
  initTestData();
  MmapBarStorage storage(g_test_dir);

  auto tf = TimeframeId::time(std::chrono::seconds(100000));

  for (auto _ : state)
  {
    auto span = storage.getBars(tf);
    double sum = 0.0;
    for (const auto& bar : span)
    {
      sum += bar.close.toDouble();
    }
    benchmark::DoNotOptimize(sum);
  }
}
BENCHMARK(BM_MmapBarStorage_IterateAllBars);

static void BM_MmapBarStorage_FindBarExact(benchmark::State& state)
{
  initTestData();
  MmapBarStorage storage(g_test_dir);

  auto tf = TimeframeId::time(std::chrono::seconds(100000));
  size_t barCount = storage.barCount(tf);

  std::mt19937 rng(42);
  std::uniform_int_distribution<int64_t> dist(0, static_cast<int64_t>(barCount - 1) * 60000);

  for (auto _ : state)
  {
    // Round to valid bar time
    int64_t ms = (dist(rng) / 60000) * 60000;
    TimePoint time{std::chrono::milliseconds(ms)};
    const Bar* bar = storage.findBar(tf, time, 'e');
    benchmark::DoNotOptimize(bar);
  }
}
BENCHMARK(BM_MmapBarStorage_FindBarExact);

static void BM_MmapBarStorage_FindBarBefore(benchmark::State& state)
{
  initTestData();
  MmapBarStorage storage(g_test_dir);

  auto tf = TimeframeId::time(std::chrono::seconds(100000));
  size_t barCount = storage.barCount(tf);

  std::mt19937 rng(42);
  std::uniform_int_distribution<int64_t> dist(60000, static_cast<int64_t>(barCount) * 60000);

  for (auto _ : state)
  {
    TimePoint time{std::chrono::milliseconds(dist(rng))};
    const Bar* bar = storage.findBar(tf, time, 'b');
    benchmark::DoNotOptimize(bar);
  }
}
BENCHMARK(BM_MmapBarStorage_FindBarBefore);

// Comparison: ifstream sequential read
static void BM_IfstreamSequentialRead(benchmark::State& state)
{
  initTestData();

  auto path = g_test_dir / "bars_100000s.bin";
  size_t barCount = 100000;

  for (auto _ : state)
  {
    std::ifstream file(path, std::ios::binary);
    uint64_t count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    double sum = 0.0;
    Bar bar;
    for (size_t i = 0; i < barCount; ++i)
    {
      file.read(reinterpret_cast<char*>(&bar), sizeof(bar));
      sum += bar.close.toDouble();
    }
    benchmark::DoNotOptimize(sum);
  }
}
BENCHMARK(BM_IfstreamSequentialRead);

// Comparison: ifstream random read
static void BM_IfstreamRandomRead(benchmark::State& state)
{
  initTestData();

  auto path = g_test_dir / "bars_100000s.bin";
  size_t barCount = 100000;

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, barCount - 1);

  std::ifstream file(path, std::ios::binary);

  for (auto _ : state)
  {
    size_t idx = dist(rng);
    file.seekg(sizeof(uint64_t) + idx * sizeof(Bar));
    Bar bar;
    file.read(reinterpret_cast<char*>(&bar), sizeof(bar));
    benchmark::DoNotOptimize(bar);
  }
}
BENCHMARK(BM_IfstreamRandomRead);

BENCHMARK_MAIN();
