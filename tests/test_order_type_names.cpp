/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Regression coverage for the shared order-type string<->code table
// (include/flox/capi/order_type_names.hpp). Every language binding's
// order-type decoder/encoder (Node's orderTypeName/orderToJs/signalToJs,
// QuickJS's jsOrderTypeName, Python's pyOrderFromC/pySignalFromC) forwards
// to these functions, so this file is the authoritative test for all of
// them at once — see CAPI-01, CAPI-09, CAPI-10, NC-06.

#include "flox/capi/flox_capi.h"
#include "flox/capi/order_type_names.hpp"
#include "flox/common.h"

#include <gtest/gtest.h>

using flox::capi::kOrderTypeNameCount;
using flox::capi::kSignalTypeNameCount;
using flox::capi::orderTypeCodeFromName;
using flox::capi::orderTypeNameLower;
using flox::capi::orderTypeNameUpper;
using flox::capi::signalTypeName;

// ── Space A: flox::OrderType ────────────────────────────────────────

TEST(OrderTypeNames, SpaceACoversTheWholeEnum)
{
  // If this ever fails, flox::OrderType grew a member that
  // order_type_names.hpp does not know about yet.
  EXPECT_EQ(kOrderTypeNameCount,
            static_cast<std::size_t>(flox::OrderType::ICEBERG) + 1);
}

TEST(OrderTypeNames, SpaceAUpperNamesMatchTheEnum)
{
  EXPECT_STREQ(orderTypeNameUpper(0), "LIMIT");
  EXPECT_STREQ(orderTypeNameUpper(1), "MARKET");
  EXPECT_STREQ(orderTypeNameUpper(2), "STOP_MARKET");
  EXPECT_STREQ(orderTypeNameUpper(3), "STOP_LIMIT");
  EXPECT_STREQ(orderTypeNameUpper(4), "TP_MARKET");
  EXPECT_STREQ(orderTypeNameUpper(5), "TP_LIMIT");
  // NC-06: code 6 used to come back "ICEBERG" (off by one).
  EXPECT_STREQ(orderTypeNameUpper(6), "TRAILING_STOP");
  // NC-06: code 7 used to fall into the default case and come back
  // "UNKNOWN".
  EXPECT_STREQ(orderTypeNameUpper(7), "ICEBERG");
  EXPECT_STREQ(orderTypeNameUpper(8), "UNKNOWN");
  EXPECT_STREQ(orderTypeNameUpper(255), "UNKNOWN");
}

TEST(OrderTypeNames, SpaceALowerNamesMatchTheEnum)
{
  EXPECT_STREQ(orderTypeNameLower(0), "limit");
  EXPECT_STREQ(orderTypeNameLower(1), "market");
  EXPECT_STREQ(orderTypeNameLower(2), "stop_market");
  EXPECT_STREQ(orderTypeNameLower(3), "stop_limit");
  EXPECT_STREQ(orderTypeNameLower(4), "tp_market");
  EXPECT_STREQ(orderTypeNameLower(5), "tp_limit");
  EXPECT_STREQ(orderTypeNameLower(6), "trailing_stop");
  EXPECT_STREQ(orderTypeNameLower(7), "iceberg");
  EXPECT_STREQ(orderTypeNameLower(8), "unknown");
}

TEST(OrderTypeNames, SpaceAEncodeRoundTripsWithDecode)
{
  for (std::size_t i = 0; i < kOrderTypeNameCount; ++i)
  {
    uint8_t code = 0xFF;
    ASSERT_TRUE(orderTypeCodeFromName(orderTypeNameLower(static_cast<uint8_t>(i)), &code))
        << "name at index " << i;
    EXPECT_EQ(code, i);
  }
}

TEST(OrderTypeNames, SpaceAEncodeMatchesFloxOrderType)
{
  // This is the exact bug: Node's SubmitOrder used to encode "market" and
  // "limit" in the FLOX_SIGNAL_TYPE_* space (market=0) and hand the result
  // to a function that reads flox::OrderType (LIMIT=0). Pin the correct
  // mapping explicitly.
  uint8_t code = 0xFF;
  ASSERT_TRUE(orderTypeCodeFromName("limit", &code));
  EXPECT_EQ(code, static_cast<uint8_t>(flox::OrderType::LIMIT));
  EXPECT_EQ(code, 0);

  ASSERT_TRUE(orderTypeCodeFromName("market", &code));
  EXPECT_EQ(code, static_cast<uint8_t>(flox::OrderType::MARKET));
  EXPECT_EQ(code, 1);

  ASSERT_TRUE(orderTypeCodeFromName("stop_market", &code));
  EXPECT_EQ(code, static_cast<uint8_t>(flox::OrderType::STOP_MARKET));

  ASSERT_TRUE(orderTypeCodeFromName("stop_limit", &code));
  EXPECT_EQ(code, static_cast<uint8_t>(flox::OrderType::STOP_LIMIT));

  ASSERT_TRUE(orderTypeCodeFromName("trailing_stop", &code));
  EXPECT_EQ(code, static_cast<uint8_t>(flox::OrderType::TRAILING_STOP));

  ASSERT_TRUE(orderTypeCodeFromName("iceberg", &code));
  EXPECT_EQ(code, static_cast<uint8_t>(flox::OrderType::ICEBERG));
}

TEST(OrderTypeNames, SpaceAEncodeRejectsUnknownNames)
{
  uint8_t code = 42;
  EXPECT_FALSE(orderTypeCodeFromName("", &code));
  EXPECT_FALSE(orderTypeCodeFromName("LIMIT", &code));   // case-sensitive
  EXPECT_FALSE(orderTypeCodeFromName("cancel", &code));  // Space B only
  EXPECT_EQ(code, 42) << "unrecognized name must not touch *out";
}

// ── Space B: FLOX_SIGNAL_TYPE_* ─────────────────────────────────────

TEST(OrderTypeNames, SpaceBCoversAllDefinedCodes)
{
  EXPECT_EQ(kSignalTypeNameCount, static_cast<std::size_t>(FLOX_SIGNAL_TYPE_ICEBERG) + 1);
}

TEST(OrderTypeNames, SpaceBNamesMatchTheWireCodes)
{
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_MARKET), "market");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_LIMIT), "limit");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_STOP_MARKET), "stop_market");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_STOP_LIMIT), "stop_limit");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_TAKE_PROFIT_MARKET), "tp_market");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT), "tp_limit");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_TRAILING_STOP), "trailing_stop");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_CANCEL), "cancel");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_CANCEL_ALL), "cancel_all");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_MODIFY), "modify");
  // CAPI-10: code 10 used to fall outside a 10-element array (`< 10`) and
  // come back "unknown".
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_ICEBERG), "iceberg");
  EXPECT_EQ(FLOX_SIGNAL_TYPE_ICEBERG, 10);
  EXPECT_STREQ(signalTypeName(11), "unknown");
  EXPECT_STREQ(signalTypeName(255), "unknown");
}

// ── Space A and Space B never agree on 0/1 by construction ─────────

TEST(OrderTypeNames, SpacesADisagreeWithBOnMarketAndLimit)
{
  // The whole reason CAPI-01/CAPI-09 exist: LIMIT and MARKET are swapped
  // between the two code spaces on purpose. A binding that decodes one
  // space's code with the other space's table gets exactly these two
  // values backwards.
  EXPECT_STRNE(orderTypeNameLower(0), signalTypeName(0));
  EXPECT_STREQ(orderTypeNameLower(0), "limit");
  EXPECT_STREQ(signalTypeName(0), "market");
  EXPECT_STRNE(orderTypeNameLower(1), signalTypeName(1));
  EXPECT_STREQ(orderTypeNameLower(1), "market");
  EXPECT_STREQ(signalTypeName(1), "limit");
}
