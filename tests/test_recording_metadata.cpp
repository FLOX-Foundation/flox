/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/recording_metadata.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace flox::replay;

class RecordingMetadataTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _testDir = std::filesystem::temp_directory_path() / "flox_metadata_test";
    std::filesystem::create_directories(_testDir);
  }

  void TearDown() override { std::filesystem::remove_all(_testDir); }

  std::filesystem::path _testDir;
};

TEST_F(RecordingMetadataTest, SaveAndLoad)
{
  RecordingMetadata meta;
  meta.recording_id = "test-recording-123";
  meta.description = "Test recording for BTCUSDT";
  meta.exchange = "binance";
  meta.exchange_type = "cex";
  meta.instrument_type = "perpetual";
  meta.connector_version = "1.2.3";
  meta.has_trades = true;
  meta.has_book_snapshots = true;
  meta.has_book_deltas = false;
  meta.book_depth = 20;
  meta.recording_start = "2025-01-15T10:30:00.000Z";
  meta.recording_end = "2025-01-15T18:45:00.000Z";
  meta.price_scale = 100000000;
  meta.qty_scale = 100000000;

  SymbolInfo sym;
  sym.symbol_id = 1;
  sym.name = "BTCUSDT";
  sym.base_asset = "BTC";
  sym.quote_asset = "USDT";
  sym.price_precision = 2;
  sym.qty_precision = 3;
  meta.symbols.push_back(sym);

  meta.custom["source"] = "live";
  meta.custom["region"] = "ap-northeast-1";

  auto path = _testDir / "metadata.json";
  ASSERT_TRUE(meta.save(path));
  ASSERT_TRUE(std::filesystem::exists(path));

  auto loaded = RecordingMetadata::load(path);
  ASSERT_TRUE(loaded.has_value());

  EXPECT_EQ(loaded->recording_id, "test-recording-123");
  EXPECT_EQ(loaded->description, "Test recording for BTCUSDT");
  EXPECT_EQ(loaded->exchange, "binance");
  EXPECT_EQ(loaded->exchange_type, "cex");
  EXPECT_EQ(loaded->instrument_type, "perpetual");
  EXPECT_EQ(loaded->connector_version, "1.2.3");
  EXPECT_TRUE(loaded->has_trades);
  EXPECT_TRUE(loaded->has_book_snapshots);
  EXPECT_FALSE(loaded->has_book_deltas);
  EXPECT_EQ(loaded->book_depth, 20);
  EXPECT_EQ(loaded->recording_start, "2025-01-15T10:30:00.000Z");
  EXPECT_EQ(loaded->recording_end, "2025-01-15T18:45:00.000Z");
  EXPECT_EQ(loaded->price_scale, 100000000);
  EXPECT_EQ(loaded->qty_scale, 100000000);
}

TEST_F(RecordingMetadataTest, MetadataPath)
{
  auto path = RecordingMetadata::metadataPath("/data/recordings/btcusdt");
  EXPECT_EQ(path.filename(), "metadata.json");
}

TEST_F(RecordingMetadataTest, LoadNonExistent)
{
  auto loaded = RecordingMetadata::load(_testDir / "nonexistent.json");
  EXPECT_FALSE(loaded.has_value());
}

TEST_F(RecordingMetadataTest, EmptyMetadata)
{
  RecordingMetadata meta;
  auto path = _testDir / "empty.json";
  ASSERT_TRUE(meta.save(path));

  auto loaded = RecordingMetadata::load(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_TRUE(loaded->exchange.empty());
  EXPECT_EQ(loaded->price_scale, 100000000);  // default
  EXPECT_EQ(loaded->qty_scale, 100000000);    // default
}

TEST_F(RecordingMetadataTest, EscapedStrings)
{
  RecordingMetadata meta;
  meta.description = "Test with \"quotes\" and\nnewlines";
  meta.exchange = "test\\exchange";

  auto path = _testDir / "escaped.json";
  ASSERT_TRUE(meta.save(path));

  auto loaded = RecordingMetadata::load(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->description, "Test with \"quotes\" and\nnewlines");
  EXPECT_EQ(loaded->exchange, "test\\exchange");
}

TEST_F(RecordingMetadataTest, MultipleSymbols)
{
  RecordingMetadata meta;
  meta.exchange = "binance";

  for (uint32_t i = 0; i < 5; ++i)
  {
    SymbolInfo sym;
    sym.symbol_id = i;
    sym.name = "SYM" + std::to_string(i);
    sym.price_precision = static_cast<int8_t>(i + 1);
    meta.symbols.push_back(sym);
  }

  auto path = _testDir / "multi_sym.json";
  ASSERT_TRUE(meta.save(path));

  // Verify JSON structure
  std::ifstream file(path);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  EXPECT_NE(content.find("\"symbols\":"), std::string::npos);
  EXPECT_NE(content.find("\"SYM0\""), std::string::npos);
  EXPECT_NE(content.find("\"SYM4\""), std::string::npos);
}

TEST_F(RecordingMetadataTest, JsonFormat)
{
  RecordingMetadata meta;
  meta.exchange = "bybit";
  meta.has_trades = true;
  meta.book_depth = 10;

  auto path = _testDir / "format.json";
  ASSERT_TRUE(meta.save(path));

  std::ifstream file(path);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // Check JSON structure
  EXPECT_NE(content.find("{"), std::string::npos);
  EXPECT_NE(content.find("}"), std::string::npos);
  EXPECT_NE(content.find("\"exchange\": \"bybit\""), std::string::npos);
  EXPECT_NE(content.find("\"has_trades\": true"), std::string::npos);
  EXPECT_NE(content.find("\"book_depth\": 10"), std::string::npos);
}
