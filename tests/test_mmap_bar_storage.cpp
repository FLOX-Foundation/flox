/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/mmap_bar_storage.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <filesystem>
#include <fstream>

using namespace flox;

class MmapBarStorageTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "flox_mmap_test";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  void writeBarsFile(const std::string& filename, const std::vector<Bar>& bars)
  {
    std::ofstream file(_test_dir / filename, std::ios::binary);
    uint64_t count = bars.size();
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    file.write(reinterpret_cast<const char*>(bars.data()), bars.size() * sizeof(Bar));
  }

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

  std::filesystem::path _test_dir;
};

TEST_F(MmapBarStorageTest, LoadSingleTimeframe)
{
  std::vector<Bar> bars = {
      makeBar(100.0, 1000),
      makeBar(101.0, 2000),
      makeBar(102.0, 3000),
  };

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);

  auto timeframes = storage.timeframes();
  ASSERT_EQ(timeframes.size(), 1);

  auto tf = TimeframeId::time(std::chrono::seconds(60));
  EXPECT_EQ(storage.barCount(tf), 3);
  EXPECT_EQ(storage.totalBars(), 3);
}

TEST_F(MmapBarStorageTest, LoadMultipleTimeframes)
{
  std::vector<Bar> bars60s = {makeBar(100.0, 1000), makeBar(101.0, 2000)};
  std::vector<Bar> bars300s = {makeBar(100.0, 5000)};

  writeBarsFile("bars_60s.bin", bars60s);
  writeBarsFile("bars_300s.bin", bars300s);

  MmapBarStorage storage(_test_dir);

  auto timeframes = storage.timeframes();
  EXPECT_EQ(timeframes.size(), 2);

  EXPECT_EQ(storage.barCount(TimeframeId::time(std::chrono::seconds(60))), 2);
  EXPECT_EQ(storage.barCount(TimeframeId::time(std::chrono::seconds(300))), 1);
  EXPECT_EQ(storage.totalBars(), 3);
}

TEST_F(MmapBarStorageTest, GetBarByIndex)
{
  std::vector<Bar> bars = {
      makeBar(100.0, 1000),
      makeBar(105.0, 2000),
      makeBar(110.0, 3000),
  };

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  const Bar* bar0 = storage.getBar(tf, 0);
  ASSERT_NE(bar0, nullptr);
  EXPECT_DOUBLE_EQ(bar0->open.toDouble(), 100.0);

  const Bar* bar1 = storage.getBar(tf, 1);
  ASSERT_NE(bar1, nullptr);
  EXPECT_DOUBLE_EQ(bar1->open.toDouble(), 105.0);

  const Bar* bar2 = storage.getBar(tf, 2);
  ASSERT_NE(bar2, nullptr);
  EXPECT_DOUBLE_EQ(bar2->open.toDouble(), 110.0);

  // Out of bounds
  EXPECT_EQ(storage.getBar(tf, 3), nullptr);
  EXPECT_EQ(storage.getBar(tf, 100), nullptr);
}

TEST_F(MmapBarStorageTest, GetBarsSpan)
{
  std::vector<Bar> bars = {
      makeBar(100.0, 1000),
      makeBar(101.0, 2000),
      makeBar(102.0, 3000),
  };

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  auto span = storage.getBars(tf);
  ASSERT_EQ(span.size(), 3);

  EXPECT_DOUBLE_EQ(span[0].open.toDouble(), 100.0);
  EXPECT_DOUBLE_EQ(span[1].open.toDouble(), 101.0);
  EXPECT_DOUBLE_EQ(span[2].open.toDouble(), 102.0);
}

TEST_F(MmapBarStorageTest, FindBarExact)
{
  std::vector<Bar> bars = {
      makeBar(100.0, 1000),
      makeBar(101.0, 2000),
      makeBar(102.0, 3000),
  };

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  // Exact match
  auto time2000 = TimePoint(std::chrono::milliseconds(2000));
  const Bar* found = storage.findBar(tf, time2000, 'e');
  ASSERT_NE(found, nullptr);
  EXPECT_DOUBLE_EQ(found->open.toDouble(), 101.0);

  // No exact match
  auto time1500 = TimePoint(std::chrono::milliseconds(1500));
  EXPECT_EQ(storage.findBar(tf, time1500, 'e'), nullptr);
}

TEST_F(MmapBarStorageTest, FindBarBefore)
{
  std::vector<Bar> bars = {
      makeBar(100.0, 1000),
      makeBar(101.0, 2000),
      makeBar(102.0, 3000),
  };

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  // Find bar before 2500ms -> should return bar at 2000ms
  auto time2500 = TimePoint(std::chrono::milliseconds(2500));
  const Bar* found = storage.findBar(tf, time2500, 'b');
  ASSERT_NE(found, nullptr);
  EXPECT_DOUBLE_EQ(found->open.toDouble(), 101.0);

  // No bar before first
  auto time500 = TimePoint(std::chrono::milliseconds(500));
  EXPECT_EQ(storage.findBar(tf, time500, 'b'), nullptr);
}

TEST_F(MmapBarStorageTest, FindBarAfterOrAt)
{
  std::vector<Bar> bars = {
      makeBar(100.0, 1000),
      makeBar(101.0, 2000),
      makeBar(102.0, 3000),
  };

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  // Find bar at or after 1500ms -> should return bar at 2000ms
  auto time1500 = TimePoint(std::chrono::milliseconds(1500));
  const Bar* found = storage.findBar(tf, time1500, 'a');
  ASSERT_NE(found, nullptr);
  EXPECT_DOUBLE_EQ(found->open.toDouble(), 101.0);

  // No bar after last
  auto time4000 = TimePoint(std::chrono::milliseconds(4000));
  EXPECT_EQ(storage.findBar(tf, time4000, 'a'), nullptr);
}

TEST_F(MmapBarStorageTest, TimeRange)
{
  std::vector<Bar> bars = {
      makeBar(100.0, 1000),
      makeBar(101.0, 2000),
      makeBar(102.0, 3000),
  };

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);

  auto [minTime, maxTime] = storage.timeRange();
  EXPECT_EQ(minTime, TimePoint(std::chrono::milliseconds(1000)));
  EXPECT_EQ(maxTime, TimePoint(std::chrono::milliseconds(3000)));
}

TEST_F(MmapBarStorageTest, NonExistentTimeframe)
{
  std::vector<Bar> bars = {makeBar(100.0, 1000)};
  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);

  auto nonExistent = TimeframeId::time(std::chrono::seconds(3600));
  EXPECT_EQ(storage.barCount(nonExistent), 0);
  EXPECT_EQ(storage.getBar(nonExistent, 0), nullptr);
  EXPECT_TRUE(storage.getBars(nonExistent).empty());
}

TEST_F(MmapBarStorageTest, EmptyDirectory)
{
  // No bars_*.bin files
  std::ofstream(_test_dir / "some_other_file.txt") << "test";

  EXPECT_THROW(MmapBarStorage storage(_test_dir), std::runtime_error);
}

TEST_F(MmapBarStorageTest, NonExistentDirectory)
{
  std::filesystem::path nonExistent = _test_dir / "does_not_exist";
  EXPECT_THROW(MmapBarStorage storage(nonExistent), std::runtime_error);
}

TEST_F(MmapBarStorageTest, MoveConstructor)
{
  std::vector<Bar> bars = {makeBar(100.0, 1000)};
  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage1(_test_dir);
  EXPECT_EQ(storage1.totalBars(), 1);

  MmapBarStorage storage2(std::move(storage1));
  EXPECT_EQ(storage2.totalBars(), 1);
}

TEST_F(MmapBarStorageTest, MoveAssignment)
{
  std::vector<Bar> bars = {makeBar(100.0, 1000)};
  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage1(_test_dir);

  // Create another directory with different data
  auto test_dir2 = std::filesystem::temp_directory_path() / "flox_mmap_test2";
  std::filesystem::create_directories(test_dir2);

  std::vector<Bar> bars2 = {makeBar(200.0, 1000), makeBar(201.0, 2000)};
  {
    std::ofstream file(test_dir2 / "bars_60s.bin", std::ios::binary);
    uint64_t count = bars2.size();
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    file.write(reinterpret_cast<const char*>(bars2.data()), bars2.size() * sizeof(Bar));
  }

  MmapBarStorage storage2(test_dir2);
  EXPECT_EQ(storage2.totalBars(), 2);

  storage2 = std::move(storage1);
  EXPECT_EQ(storage2.totalBars(), 1);

  std::filesystem::remove_all(test_dir2);
}

TEST_F(MmapBarStorageTest, LargeBarsFile)
{
  const size_t numBars = 10000;
  std::vector<Bar> bars;
  bars.reserve(numBars);

  for (size_t i = 0; i < numBars; ++i)
  {
    bars.push_back(makeBar(100.0 + static_cast<double>(i) * 0.01, static_cast<int64_t>(i) * 60000));
  }

  writeBarsFile("bars_60s.bin", bars);

  MmapBarStorage storage(_test_dir);
  EXPECT_EQ(storage.totalBars(), numBars);

  // Random access should be fast with mmap
  const Bar* bar5000 = storage.getBar(TimeframeId::time(std::chrono::seconds(60)), 5000);
  ASSERT_NE(bar5000, nullptr);
  EXPECT_DOUBLE_EQ(bar5000->open.toDouble(), 100.0 + 5000 * 0.01);
}
