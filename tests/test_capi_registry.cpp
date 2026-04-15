/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <cstring>

TEST(CapiRegistryTest, CreateAndDestroy)
{
  FloxRegistryHandle reg = flox_registry_create();
  ASSERT_NE(reg, nullptr);
  flox_registry_destroy(reg);
}

TEST(CapiRegistryTest, AddSymbolAndCount)
{
  FloxRegistryHandle reg = flox_registry_create();

  EXPECT_EQ(flox_registry_symbol_count(reg), 0u);

  uint32_t id1 = flox_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);
  EXPECT_GT(id1, 0u);
  EXPECT_EQ(flox_registry_symbol_count(reg), 1u);

  uint32_t id2 = flox_registry_add_symbol(reg, "Binance", "ETHUSDT", 0.01);
  EXPECT_NE(id1, id2);
  EXPECT_EQ(flox_registry_symbol_count(reg), 2u);

  flox_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolId)
{
  FloxRegistryHandle reg = flox_registry_create();

  uint32_t added_id = flox_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  uint32_t found_id = 0;
  EXPECT_EQ(flox_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &found_id), 1);
  EXPECT_EQ(found_id, added_id);

  // Not found
  uint32_t missing_id = 0;
  EXPECT_EQ(flox_registry_get_symbol_id(reg, "Binance", "DOGEUSDT", &missing_id), 0);

  // Wrong exchange
  EXPECT_EQ(flox_registry_get_symbol_id(reg, "Bybit", "BTCUSDT", &missing_id), 0);

  flox_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolName)
{
  FloxRegistryHandle reg = flox_registry_create();

  uint32_t id = flox_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  char exchange[64] = {};
  char name[64] = {};
  EXPECT_EQ(flox_registry_get_symbol_name(reg, id, exchange, sizeof(exchange), name, sizeof(name)),
            1);
  EXPECT_STREQ(exchange, "Binance");
  EXPECT_STREQ(name, "BTCUSDT");

  // Invalid symbol ID
  EXPECT_EQ(flox_registry_get_symbol_name(reg, 9999, exchange, sizeof(exchange), name,
                                          sizeof(name)),
            0);

  flox_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolNameTruncation)
{
  FloxRegistryHandle reg = flox_registry_create();

  flox_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  uint32_t id = 0;
  flox_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &id);

  // Small buffer — should truncate safely
  char exchange[4] = {};
  char name[4] = {};
  EXPECT_EQ(flox_registry_get_symbol_name(reg, id, exchange, sizeof(exchange), name, sizeof(name)),
            1);
  EXPECT_STREQ(exchange, "Bin");
  EXPECT_STREQ(name, "BTC");

  flox_registry_destroy(reg);
}

TEST(CapiRegistryTest, MultipleExchangesSameSymbol)
{
  FloxRegistryHandle reg = flox_registry_create();

  uint32_t binance_id = flox_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);
  uint32_t bybit_id = flox_registry_add_symbol(reg, "Bybit", "BTCUSDT", 0.01);

  EXPECT_NE(binance_id, bybit_id);

  uint32_t found = 0;
  EXPECT_EQ(flox_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &found), 1);
  EXPECT_EQ(found, binance_id);

  EXPECT_EQ(flox_registry_get_symbol_id(reg, "Bybit", "BTCUSDT", &found), 1);
  EXPECT_EQ(found, bybit_id);

  flox_registry_destroy(reg);
}
