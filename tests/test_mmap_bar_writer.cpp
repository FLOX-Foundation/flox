/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/mmap_bar_storage.h"
#include "flox/backtest/mmap_bar_writer.h"

#include <gtest/gtest.h>
#include <filesystem>

using namespace flox;

class MmapBarWriterTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _testDir = std::filesystem::temp_directory_path() / "flox_bar_writer_test";
    std::filesystem::remove_all(_testDir);
    std::filesystem::create_directories(_testDir);
  }

  void TearDown() override { std::filesystem::remove_all(_testDir); }

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

  std::filesystem::path _testDir;
};

TEST_F(MmapBarWriterTest, WriteAndReadBars)
{
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  // Write bars using writer
  {
    MmapBarWriter writer(_testDir);
    std::vector<Bar> bars = {makeBar(100.0, 60000), makeBar(101.0, 120000), makeBar(102.0, 180000)};
    writer.writeBars(tf, bars);
  }

  // Read using storage
  MmapBarStorage storage(_testDir);
  EXPECT_EQ(storage.totalBars(), 3);
  EXPECT_EQ(storage.barCount(tf), 3);

  auto* bar0 = storage.getBar(tf, 0);
  ASSERT_NE(bar0, nullptr);
  EXPECT_DOUBLE_EQ(bar0->open.toDouble(), 100.0);

  auto* bar2 = storage.getBar(tf, 2);
  ASSERT_NE(bar2, nullptr);
  EXPECT_DOUBLE_EQ(bar2->open.toDouble(), 102.0);
}

TEST_F(MmapBarWriterTest, BufferAndFlush)
{
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  {
    MmapBarWriter writer(_testDir);

    BarEvent ev;
    ev.barType = BarType::Time;
    ev.barTypeParam = 60'000'000'000ULL;
    ev.bar = makeBar(100.0, 60000);

    writer.onBar(ev);
    EXPECT_EQ(writer.bufferedBars(tf), 1);
    EXPECT_EQ(writer.totalBufferedBars(), 1);

    ev.bar = makeBar(101.0, 120000);
    writer.onBar(ev);
    EXPECT_EQ(writer.bufferedBars(tf), 2);

    // File should not exist yet
    EXPECT_FALSE(std::filesystem::exists(_testDir / "bars_60s.bin"));

    writer.flush();
    EXPECT_EQ(writer.bufferedBars(tf), 0);
    EXPECT_TRUE(std::filesystem::exists(_testDir / "bars_60s.bin"));
  }

  // Verify written data
  MmapBarStorage storage(_testDir);
  EXPECT_EQ(storage.barCount(tf), 2);
}

TEST_F(MmapBarWriterTest, AppendOnFlush)
{
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  // First batch
  {
    MmapBarWriter writer(_testDir);
    std::vector<Bar> bars = {makeBar(100.0, 60000), makeBar(101.0, 120000)};
    writer.writeBars(tf, bars);
  }

  // Second batch - should append
  {
    MmapBarWriter writer(_testDir);

    BarEvent ev;
    ev.barType = BarType::Time;
    ev.barTypeParam = 60'000'000'000ULL;
    ev.bar = makeBar(102.0, 180000);
    writer.onBar(ev);

    ev.bar = makeBar(103.0, 240000);
    writer.onBar(ev);

    writer.flush();
  }

  // Verify all 4 bars
  MmapBarStorage storage(_testDir);
  EXPECT_EQ(storage.barCount(tf), 4);

  auto* bar0 = storage.getBar(tf, 0);
  EXPECT_DOUBLE_EQ(bar0->open.toDouble(), 100.0);

  auto* bar3 = storage.getBar(tf, 3);
  EXPECT_DOUBLE_EQ(bar3->open.toDouble(), 103.0);
}

TEST_F(MmapBarWriterTest, MultipleTimeframes)
{
  auto tf60 = TimeframeId::time(std::chrono::seconds(60));
  auto tf300 = TimeframeId::time(std::chrono::seconds(300));

  {
    MmapBarWriter writer(_testDir);

    BarEvent ev60;
    ev60.barType = BarType::Time;
    ev60.barTypeParam = 60'000'000'000ULL;
    ev60.bar = makeBar(100.0, 60000);
    writer.onBar(ev60);

    ev60.bar = makeBar(101.0, 120000);
    writer.onBar(ev60);

    BarEvent ev300;
    ev300.barType = BarType::Time;
    ev300.barTypeParam = 300'000'000'000ULL;
    ev300.bar = makeBar(100.5, 300000);
    writer.onBar(ev300);

    EXPECT_EQ(writer.bufferedBars(tf60), 2);
    EXPECT_EQ(writer.bufferedBars(tf300), 1);
    EXPECT_EQ(writer.totalBufferedBars(), 3);

    writer.flush();
  }

  MmapBarStorage storage(_testDir);
  EXPECT_EQ(storage.barCount(tf60), 2);
  EXPECT_EQ(storage.barCount(tf300), 1);
  EXPECT_EQ(storage.totalBars(), 3);
}

TEST_F(MmapBarWriterTest, ClearBuffers)
{
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  MmapBarWriter writer(_testDir);

  BarEvent ev;
  ev.barType = BarType::Time;
  ev.barTypeParam = 60'000'000'000ULL;
  ev.bar = makeBar(100.0, 60000);
  writer.onBar(ev);

  EXPECT_EQ(writer.totalBufferedBars(), 1);

  writer.clear();

  EXPECT_EQ(writer.totalBufferedBars(), 0);
  EXPECT_FALSE(std::filesystem::exists(_testDir / "bars_60s.bin"));
}

TEST_F(MmapBarWriterTest, CreatesDirectoryIfNotExists)
{
  auto newDir = _testDir / "subdir" / "data";
  EXPECT_FALSE(std::filesystem::exists(newDir));

  MmapBarWriter writer(newDir);
  EXPECT_TRUE(std::filesystem::exists(newDir));
}

TEST_F(MmapBarWriterTest, AutoFlushOnDestruction)
{
  auto tf = TimeframeId::time(std::chrono::seconds(60));

  {
    MmapBarWriter writer(_testDir);

    BarEvent ev;
    ev.barType = BarType::Time;
    ev.barTypeParam = 60'000'000'000ULL;
    ev.bar = makeBar(100.0, 60000);
    writer.onBar(ev);

    // No explicit flush - should flush on destruction
  }

  EXPECT_TRUE(std::filesystem::exists(_testDir / "bars_60s.bin"));

  MmapBarStorage storage(_testDir);
  EXPECT_EQ(storage.barCount(tf), 1);
}

TEST_F(MmapBarWriterTest, WriteMetadata)
{
  MmapBarWriter writer(_testDir);

  writer.setMetadata("symbol", "BTCUSDT");
  writer.setMetadata("symbol_id", "1");
  writer.writeMetadata();

  auto metadataPath = _testDir / ".symbol_metadata";
  EXPECT_TRUE(std::filesystem::exists(metadataPath));

  std::ifstream file(metadataPath);
  std::map<std::string, std::string> metadata;
  std::string line;
  while (std::getline(file, line))
  {
    auto pos = line.find('=');
    if (pos != std::string::npos)
    {
      metadata[line.substr(0, pos)] = line.substr(pos + 1);
    }
  }

  EXPECT_EQ(metadata["symbol"], "BTCUSDT");
  EXPECT_EQ(metadata["symbol_id"], "1");
}

TEST_F(MmapBarWriterTest, SetMetadataFromMap)
{
  MmapBarWriter writer(_testDir);

  std::map<std::string, std::string> meta = {{"symbol", "ETHUSDT"}, {"symbol_id", "2"}};
  writer.setMetadata(meta);
  writer.writeMetadata();

  std::ifstream file(_testDir / ".symbol_metadata");
  std::map<std::string, std::string> metadata;
  std::string line;
  while (std::getline(file, line))
  {
    auto pos = line.find('=');
    if (pos != std::string::npos)
    {
      metadata[line.substr(0, pos)] = line.substr(pos + 1);
    }
  }

  EXPECT_EQ(metadata["symbol"], "ETHUSDT");
  EXPECT_EQ(metadata["symbol_id"], "2");
}
