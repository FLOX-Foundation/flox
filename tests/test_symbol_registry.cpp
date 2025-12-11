/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/symbol_registry.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace flox;

TEST(SymbolRegistryTest, RegisterAndGetSymbolId)
{
  SymbolRegistry registry;

  auto id1 = registry.registerSymbol("bybit", "BTCUSDT");
  auto id2 = registry.registerSymbol("binance", "ETHUSDT");
  auto id3 = registry.registerSymbol("bybit", "BTCUSDT");

  EXPECT_EQ(id1, id3);
  EXPECT_NE(id1, id2);

  auto maybeId = registry.getSymbolId("bybit", "BTCUSDT");
  ASSERT_TRUE(maybeId.has_value());
  EXPECT_EQ(maybeId.value(), id1);

  auto missingId = registry.getSymbolId("bybit", "DOGEUSDT");
  EXPECT_FALSE(missingId.has_value());
}

TEST(SymbolRegistryTest, GetSymbolName)
{
  SymbolRegistry registry;
  auto id = registry.registerSymbol("bybit", "BTCUSDT");

  auto [exchange, symbol] = registry.getSymbolName(id);
  EXPECT_EQ(exchange, "bybit");
  EXPECT_EQ(symbol, "BTCUSDT");
}

TEST(SymbolRegistryTest, ThreadSafety)
{
  SymbolRegistry registry;
  constexpr int threadCount = 8;
  constexpr int symbolsPerThread = 1000;

  std::vector<std::thread> threads;

  for (int t = 0; t < threadCount; ++t)
  {
    threads.emplace_back(
        [t, &registry]()
        {
          for (int i = 0; i < symbolsPerThread; ++i)
          {
            std::string exchange = "ex" + std::to_string(t % 3);
            std::string symbol = "SYM_" + std::to_string(t) + "_" + std::to_string(i);
            registry.registerSymbol(exchange, symbol);
          }
        });
  }

  for (auto& th : threads)
  {
    th.join();
  }

  int total = threadCount * symbolsPerThread;
  std::unordered_set<SymbolId> ids;
  for (int t = 0; t < threadCount; ++t)
  {
    for (int i = 0; i < symbolsPerThread; ++i)
    {
      std::string exchange = "ex" + std::to_string(t % 3);
      std::string symbol = "SYM_" + std::to_string(t) + "_" + std::to_string(i);
      auto maybeId = registry.getSymbolId(exchange, symbol);
      ASSERT_TRUE(maybeId.has_value());
      ids.insert(maybeId.value());
    }
  }

  EXPECT_EQ(ids.size(), total);
}

TEST(SymbolRegistryTest, StressTestMassiveSymbols)
{
  SymbolRegistry registry;
  constexpr int count = 100000;

  for (int i = 0; i < count; ++i)
  {
    std::string symbol = "S" + std::to_string(i);
    registry.registerSymbol("stress", symbol);
  }

  for (int i = 0; i < count; ++i)
  {
    std::string symbol = "S" + std::to_string(i);
    auto id = registry.getSymbolId("stress", symbol);
    ASSERT_TRUE(id.has_value());
    auto [ex, sym] = registry.getSymbolName(*id);
    EXPECT_EQ(ex, "stress");
    EXPECT_EQ(sym, symbol);
  }
}

TEST(SymbolRegistryTest, RegisterOptionAndFutureSymbols)
{
  SymbolRegistry registry;

  SymbolInfo option;
  option.exchange = "deribit";
  option.symbol = "BTC-30AUG24-50000-C";
  option.type = InstrumentType::Option;
  option.strike = Price::fromDouble(50000.0);
  option.optionType = OptionType::CALL;

  SymbolInfo future;
  future.exchange = "deribit";
  future.symbol = "BTC-30AUG24";
  future.type = InstrumentType::Future;

  auto optId = registry.registerSymbol(option);
  auto futId = registry.registerSymbol(future);

  EXPECT_NE(optId, futId);

  auto optInfo = registry.getSymbolInfo(optId);
  EXPECT_TRUE(optInfo.has_value());
  EXPECT_EQ(optInfo->type, InstrumentType::Option);
  EXPECT_EQ(optInfo->optionType.value(), OptionType::CALL);
  EXPECT_EQ(optInfo->strike.value(), Price::fromDouble(50000.0));

  auto futOptInfo = registry.getSymbolInfo(futId);
  EXPECT_TRUE(futOptInfo.has_value());
  EXPECT_EQ(futOptInfo->type, InstrumentType::Future);
}

TEST(SymbolRegistryTest, SaveAndLoadFromFile)
{
  std::filesystem::path tempPath = std::filesystem::temp_directory_path() / "test_registry.json";

  // Cleanup before test
  std::filesystem::remove(tempPath);

  {
    SymbolRegistry registry;

    // Register simple symbols using SymbolInfo
    SymbolInfo btc;
    btc.exchange = "binance";
    btc.symbol = "BTCUSDT";
    btc.type = InstrumentType::Spot;
    registry.registerSymbol(btc);

    SymbolInfo eth;
    eth.exchange = "binance";
    eth.symbol = "ETHUSDT";
    eth.type = InstrumentType::Spot;
    registry.registerSymbol(eth);

    // Register option with all fields
    SymbolInfo option;
    option.exchange = "deribit";
    option.symbol = "BTC-30AUG24-50000-C";
    option.type = InstrumentType::Option;
    option.strike = Price::fromDouble(50000.0);
    option.expiry = TimePoint(std::chrono::nanoseconds(1724976000000000000LL));  // Aug 30, 2024
    option.optionType = OptionType::CALL;
    registry.registerSymbol(option);

    // Register future
    SymbolInfo future;
    future.exchange = "deribit";
    future.symbol = "BTC-30AUG24";
    future.type = InstrumentType::Future;
    future.expiry = TimePoint(std::chrono::nanoseconds(1724976000000000000LL));
    registry.registerSymbol(future);

    EXPECT_EQ(registry.size(), 4);
    EXPECT_TRUE(registry.saveToFile(tempPath));
  }

  // Verify file exists
  EXPECT_TRUE(std::filesystem::exists(tempPath));

  {
    SymbolRegistry registry;
    EXPECT_TRUE(registry.loadFromFile(tempPath));
    EXPECT_EQ(registry.size(), 4);

    // Verify symbols loaded correctly
    auto symbols = registry.getAllSymbols();
    EXPECT_EQ(symbols.size(), 4);

    // Check option was loaded with all fields
    bool foundOption = false;
    for (const auto& sym : symbols)
    {
      if (sym.symbol == "BTC-30AUG24-50000-C")
      {
        foundOption = true;
        EXPECT_EQ(sym.exchange, "deribit");
        EXPECT_EQ(sym.type, InstrumentType::Option);
        EXPECT_TRUE(sym.strike.has_value());
        EXPECT_EQ(sym.strike->raw(), Price::fromDouble(50000.0).raw());
        EXPECT_TRUE(sym.expiry.has_value());
        EXPECT_TRUE(sym.optionType.has_value());
        EXPECT_EQ(*sym.optionType, OptionType::CALL);
      }
    }
    EXPECT_TRUE(foundOption);
  }

  // Cleanup
  std::filesystem::remove(tempPath);
}

TEST(SymbolRegistryTest, BinarySerializeDeserialize)
{
  SymbolRegistry registry;

  // Register various symbols using SymbolInfo
  SymbolInfo btc;
  btc.exchange = "binance";
  btc.symbol = "BTCUSDT";
  btc.type = InstrumentType::Spot;
  registry.registerSymbol(btc);

  SymbolInfo eth;
  eth.exchange = "bybit";
  eth.symbol = "ETHUSDT";
  eth.type = InstrumentType::Spot;
  registry.registerSymbol(eth);

  SymbolInfo option;
  option.exchange = "deribit";
  option.symbol = "ETH-PUT-2000";
  option.type = InstrumentType::Option;
  option.strike = Price::fromDouble(2000.0);
  option.expiry = TimePoint(std::chrono::nanoseconds(1735689600000000000LL));  // Jan 1, 2025
  option.optionType = OptionType::PUT;
  registry.registerSymbol(option);

  EXPECT_EQ(registry.size(), 3);

  // Serialize
  auto data = registry.serialize();
  EXPECT_GT(data.size(), 12);  // At least header size

  // Deserialize into new registry
  SymbolRegistry registry2;
  EXPECT_TRUE(registry2.deserialize(data));
  EXPECT_EQ(registry2.size(), 3);

  // Verify content
  auto symbols = registry2.getAllSymbols();
  EXPECT_EQ(symbols.size(), 3);

  // Check option preserved correctly
  bool foundOption = false;
  for (const auto& sym : symbols)
  {
    if (sym.symbol == "ETH-PUT-2000")
    {
      foundOption = true;
      EXPECT_EQ(sym.type, InstrumentType::Option);
      EXPECT_TRUE(sym.strike.has_value());
      EXPECT_EQ(sym.strike->raw(), Price::fromDouble(2000.0).raw());
      EXPECT_TRUE(sym.optionType.has_value());
      EXPECT_EQ(*sym.optionType, OptionType::PUT);
    }
  }
  EXPECT_TRUE(foundOption);
}

TEST(SymbolRegistryTest, ClearRegistry)
{
  SymbolRegistry registry;

  SymbolInfo btc;
  btc.exchange = "binance";
  btc.symbol = "BTCUSDT";
  btc.type = InstrumentType::Spot;
  registry.registerSymbol(btc);

  SymbolInfo eth;
  eth.exchange = "binance";
  eth.symbol = "ETHUSDT";
  eth.type = InstrumentType::Spot;
  registry.registerSymbol(eth);

  EXPECT_EQ(registry.size(), 2);

  registry.clear();
  EXPECT_EQ(registry.size(), 0);
  EXPECT_TRUE(registry.getAllSymbols().empty());
}

TEST(SymbolRegistryTest, EmptyRegistrySerialization)
{
  SymbolRegistry registry;
  EXPECT_EQ(registry.size(), 0);

  auto data = registry.serialize();
  EXPECT_EQ(data.size(), 12);  // Only header: magic(4) + version(4) + count(4)

  SymbolRegistry registry2;
  SymbolInfo test;
  test.exchange = "test";
  test.symbol = "TEST";
  test.type = InstrumentType::Spot;
  registry2.registerSymbol(test);
  EXPECT_EQ(registry2.size(), 1);

  EXPECT_TRUE(registry2.deserialize(data));
  EXPECT_EQ(registry2.size(), 0);  // Should be empty after deserialize
}

TEST(SymbolRegistryTest, LargeRegistrySerialization)
{
  SymbolRegistry registry;
  constexpr int count = 10000;

  for (int i = 0; i < count; ++i)
  {
    SymbolInfo info;
    info.exchange = "exchange" + std::to_string(i % 10);
    info.symbol = "SYMBOL" + std::to_string(i);
    info.type = static_cast<InstrumentType>(i % 4);
    if (i % 3 == 0)
    {
      info.strike = Price::fromDouble(100.0 * i);
    }
    registry.registerSymbol(info);
  }

  EXPECT_EQ(registry.size(), count);

  auto data = registry.serialize();
  EXPECT_GT(data.size(), 100000);  // Should be substantial

  SymbolRegistry registry2;
  EXPECT_TRUE(registry2.deserialize(data));
  EXPECT_EQ(registry2.size(), count);

  // Spot check some symbols
  auto symbols = registry2.getAllSymbols();
  EXPECT_EQ(symbols.size(), count);

  for (int i = 0; i < count; i += 1000)
  {
    EXPECT_EQ(symbols[i].symbol, "SYMBOL" + std::to_string(i));
    EXPECT_EQ(symbols[i].type, static_cast<InstrumentType>(i % 4));
  }
}
