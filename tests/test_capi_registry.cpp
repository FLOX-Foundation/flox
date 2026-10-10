/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>
#include <cstring>

TEST(CapiRegistryTest, CreateAndDestroy)
{
  LrvxRegistryHandle reg = lrvx_registry_create();
  ASSERT_NE(reg, nullptr);
  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, AddSymbolAndCount)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  EXPECT_EQ(lrvx_registry_symbol_count(reg), 0u);

  uint32_t id1 = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);
  EXPECT_GT(id1, 0u);
  EXPECT_EQ(lrvx_registry_symbol_count(reg), 1u);

  uint32_t id2 = lrvx_registry_add_symbol(reg, "Binance", "ETHUSDT", 0.01);
  EXPECT_NE(id1, id2);
  EXPECT_EQ(lrvx_registry_symbol_count(reg), 2u);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolId)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  uint32_t added_id = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  uint32_t found_id = 0;
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &found_id), 1);
  EXPECT_EQ(found_id, added_id);

  // Not found
  uint32_t missing_id = 0;
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Binance", "DOGEUSDT", &missing_id), 0);

  // Wrong exchange
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Bybit", "BTCUSDT", &missing_id), 0);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolName)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  uint32_t id = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  char exchange[64] = {};
  char name[64] = {};
  EXPECT_EQ(lrvx_registry_get_symbol_name(reg, id, exchange, sizeof(exchange), name, sizeof(name)),
            1);
  EXPECT_STREQ(exchange, "Binance");
  EXPECT_STREQ(name, "BTCUSDT");

  // Invalid symbol ID
  EXPECT_EQ(lrvx_registry_get_symbol_name(reg, 9999, exchange, sizeof(exchange), name,
                                          sizeof(name)),
            0);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolNameTruncation)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  uint32_t id = 0;
  lrvx_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &id);

  // Small buffer — should truncate safely
  char exchange[4] = {};
  char name[4] = {};
  EXPECT_EQ(lrvx_registry_get_symbol_name(reg, id, exchange, sizeof(exchange), name, sizeof(name)),
            1);
  EXPECT_STREQ(exchange, "Bin");
  EXPECT_STREQ(name, "BTC");

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, MultipleExchangesSameSymbol)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  uint32_t binance_id = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);
  uint32_t bybit_id = lrvx_registry_add_symbol(reg, "Bybit", "BTCUSDT", 0.01);

  EXPECT_NE(binance_id, bybit_id);

  uint32_t found = 0;
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &found), 1);
  EXPECT_EQ(found, binance_id);

  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Bybit", "BTCUSDT", &found), 1);
  EXPECT_EQ(found, bybit_id);

  lrvx_registry_destroy(reg);
}

// An unregistered SymbolId reaches the C API as a plain integer the caller
// invented, and Strategy's base constructor throws std::invalid_argument for
// it. Under extern "C" that exception has nowhere to go, so before the
// try/catch in lrvx_strategy_create this call terminated the process instead
// of returning a handle the caller could test.
TEST(CapiRegistryTest, StrategyCreateReturnsNullForUnregisteredSymbol)
{
  LrvxRegistryHandle reg = lrvx_registry_create();
  ASSERT_NE(reg, nullptr);

  const uint32_t bogus[] = {4242u};
  LrvxStrategyCallbacks cb{};
  LrvxStrategyHandle strat = lrvx_strategy_create(1, bogus, 1, reg, cb);

  EXPECT_EQ(strat, nullptr);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, StrategyCreateRejectsNullRegistry)
{
  const uint32_t syms[] = {1u};
  LrvxStrategyCallbacks cb{};
  EXPECT_EQ(lrvx_strategy_create(1, syms, 1, nullptr, cb), nullptr);
}
