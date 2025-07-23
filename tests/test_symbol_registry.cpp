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

  const SymbolInfo* optInfo = registry.getSymbolInfo(optId);
  ASSERT_NE(optInfo, nullptr);
  EXPECT_EQ(optInfo->type, InstrumentType::Option);
  EXPECT_EQ(optInfo->optionType.value(), OptionType::CALL);
  EXPECT_EQ(optInfo->strike.value(), Price::fromDouble(50000.0));

  const SymbolInfo* futInfo = registry.getSymbolInfo(futId);
  ASSERT_NE(futInfo, nullptr);
  EXPECT_EQ(futInfo->type, InstrumentType::Future);
}
