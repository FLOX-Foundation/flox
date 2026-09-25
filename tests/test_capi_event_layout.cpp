/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The Codon binding has no struct declarations to work from -- the codegen
// emits every out-struct as an opaque cobj -- so codon/flox/dispatch.codon
// reads FloxTradeData, FloxBarData, FloxOrderEventData and FloxSymbolContext
// by integer byte offsets typed in by hand, and codon/flox/strategy.codon
// does the same for FloxBar. The offsets are right today. Nothing in the
// build says so: add a field, widen one, reorder two, and every strategy
// written in Codon starts reading a different field than it asks for, with
// no diagnostic anywhere.
//
// The generated table in flox_capi_layout.h is what the Codon side is built
// from. This test is the other end of it: every entry is compared against
// offsetof/sizeof on the real struct, so a layout change that does not reach
// the table fails here rather than in a user's strategy.

#include "flox/capi/flox_capi.h"

#if __has_include("flox/capi/flox_capi_layout.h")
#include "flox/capi/flox_capi_layout.h"
#define FLOX_HAS_LAYOUT_TABLE 1
#else
#define FLOX_HAS_LAYOUT_TABLE 0
#endif

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct Field
{
  const char* structName;
  const char* fieldName;
  std::size_t offset;
  std::size_t size;
};

#define FLOX_FIELD(S, F) \
  Field { #S, #F, offsetof(S, F), sizeof(S::F) }

// Every field of every struct the Codon binding reads. Padding is left out:
// a table may carry it, nothing may depend on it.
const std::vector<Field>& expectedFields()
{
  static const std::vector<Field> kFields = {
      FLOX_FIELD(FloxTradeData, symbol),
      FLOX_FIELD(FloxTradeData, price_raw),
      FLOX_FIELD(FloxTradeData, quantity_raw),
      FLOX_FIELD(FloxTradeData, is_buy),
      FLOX_FIELD(FloxTradeData, exchange_ts_ns),

      FLOX_FIELD(FloxBookSnapshot, bid_price_raw),
      FLOX_FIELD(FloxBookSnapshot, bid_qty_raw),
      FLOX_FIELD(FloxBookSnapshot, ask_price_raw),
      FLOX_FIELD(FloxBookSnapshot, ask_qty_raw),
      FLOX_FIELD(FloxBookSnapshot, mid_raw),
      FLOX_FIELD(FloxBookSnapshot, spread_raw),

      FLOX_FIELD(FloxSymbolContext, symbol_id),
      FLOX_FIELD(FloxSymbolContext, has_avg_entry_price),
      FLOX_FIELD(FloxSymbolContext, position_raw),
      FLOX_FIELD(FloxSymbolContext, avg_entry_price_raw),
      FLOX_FIELD(FloxSymbolContext, last_trade_price_raw),
      FLOX_FIELD(FloxSymbolContext, last_update_ns),
      FLOX_FIELD(FloxSymbolContext, book),

      FLOX_FIELD(FloxBarData, symbol),
      FLOX_FIELD(FloxBarData, bar_type),
      FLOX_FIELD(FloxBarData, close_reason),
      FLOX_FIELD(FloxBarData, bar_type_param),
      FLOX_FIELD(FloxBarData, open_raw),
      FLOX_FIELD(FloxBarData, high_raw),
      FLOX_FIELD(FloxBarData, low_raw),
      FLOX_FIELD(FloxBarData, close_raw),
      FLOX_FIELD(FloxBarData, volume_raw),
      FLOX_FIELD(FloxBarData, buy_volume_raw),
      FLOX_FIELD(FloxBarData, trade_count_raw),
      FLOX_FIELD(FloxBarData, start_time_ns),
      FLOX_FIELD(FloxBarData, end_time_ns),

      FLOX_FIELD(FloxOrderEventData, order_id),
      FLOX_FIELD(FloxOrderEventData, symbol_id),
      FLOX_FIELD(FloxOrderEventData, side),
      FLOX_FIELD(FloxOrderEventData, order_type),
      FLOX_FIELD(FloxOrderEventData, status),
      FLOX_FIELD(FloxOrderEventData, fill_qty_raw),
      FLOX_FIELD(FloxOrderEventData, fill_price_raw),
      FLOX_FIELD(FloxOrderEventData, exchange_ts_ns),
      FLOX_FIELD(FloxOrderEventData, reject_reason),
      FLOX_FIELD(FloxOrderEventData, queue_ahead_raw),
      FLOX_FIELD(FloxOrderEventData, queue_total_raw),
      FLOX_FIELD(FloxOrderEventData, submitted_at_ns),
      FLOX_FIELD(FloxOrderEventData, accepted_at_ns),
      FLOX_FIELD(FloxOrderEventData, first_fill_at_ns),
      FLOX_FIELD(FloxOrderEventData, last_fill_at_ns),
      FLOX_FIELD(FloxOrderEventData, canceled_at_ns),
      FLOX_FIELD(FloxOrderEventData, rejected_at_ns),
      FLOX_FIELD(FloxOrderEventData, triggered_at_ns),
      FLOX_FIELD(FloxOrderEventData, expired_at_ns),
      FLOX_FIELD(FloxOrderEventData, is_maker),
      FLOX_FIELD(FloxOrderEventData, market_position),
      FLOX_FIELD(FloxOrderEventData, distance_to_best_ticks),

      FLOX_FIELD(FloxBar, start_time_ns),
      FLOX_FIELD(FloxBar, end_time_ns),
      FLOX_FIELD(FloxBar, open_raw),
      FLOX_FIELD(FloxBar, high_raw),
      FLOX_FIELD(FloxBar, low_raw),
      FLOX_FIELD(FloxBar, close_raw),
      FLOX_FIELD(FloxBar, volume_raw),
      FLOX_FIELD(FloxBar, buy_volume_raw),
      FLOX_FIELD(FloxBar, trade_count),
      FLOX_FIELD(FloxBar, close_reason),
  };
  return kFields;
}

#undef FLOX_FIELD

struct Struct
{
  const char* name;
  std::size_t size;
  std::size_t alignment;
};

#define FLOX_STRUCT(S) \
  Struct { #S, sizeof(S), alignof(S) }

const std::vector<Struct>& expectedStructs()
{
  static const std::vector<Struct> kStructs = {
      FLOX_STRUCT(FloxTradeData),
      FLOX_STRUCT(FloxBookSnapshot),
      FLOX_STRUCT(FloxSymbolContext),
      FLOX_STRUCT(FloxBarData),
      FLOX_STRUCT(FloxOrderEventData),
      FLOX_STRUCT(FloxBar),
  };
  return kStructs;
}

#undef FLOX_STRUCT

#if FLOX_HAS_LAYOUT_TABLE
std::size_t fieldCount()
{
  return sizeof(kFloxEventLayout) / sizeof(kFloxEventLayout[0]);
}

std::size_t structCount()
{
  return sizeof(kFloxEventStructLayout) / sizeof(kFloxEventStructLayout[0]);
}

const FloxFieldLayout* findField(const char* structName, const char* fieldName)
{
  for (std::size_t i = 0; i < fieldCount(); ++i)
  {
    if (std::strcmp(kFloxEventLayout[i].struct_name, structName) == 0 &&
        std::strcmp(kFloxEventLayout[i].field_name, fieldName) == 0)
    {
      return &kFloxEventLayout[i];
    }
  }
  return nullptr;
}

const FloxStructLayout* findStruct(const char* name)
{
  for (std::size_t i = 0; i < structCount(); ++i)
  {
    if (std::strcmp(kFloxEventStructLayout[i].struct_name, name) == 0)
    {
      return &kFloxEventStructLayout[i];
    }
  }
  return nullptr;
}
#endif

constexpr const char* kMissingTable =
    "include/flox/capi/flox_capi_layout.h is missing: the Codon binding's "
    "struct offsets are generated from it and this test is what checks them "
    "against offsetof";

}  // namespace

TEST(CapiEventLayout, TableCarriesEveryFieldCodonCanRead)
{
#if !FLOX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (const auto& f : expectedFields())
  {
    EXPECT_NE(findField(f.structName, f.fieldName), nullptr)
        << f.structName << "." << f.fieldName << " has no layout entry";
  }
#endif
}

TEST(CapiEventLayout, EveryOffsetAndWidthMatchesTheStruct)
{
#if !FLOX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (const auto& f : expectedFields())
  {
    const FloxFieldLayout* entry = findField(f.structName, f.fieldName);
    if (entry == nullptr)
    {
      continue;  // reported by TableCarriesEveryFieldCodonCanRead
    }
    EXPECT_EQ(static_cast<std::size_t>(entry->offset), f.offset)
        << f.structName << "." << f.fieldName << " moved";
    EXPECT_EQ(static_cast<std::size_t>(entry->size), f.size)
        << f.structName << "." << f.fieldName << " changed width";
  }
#endif
}

TEST(CapiEventLayout, StructSizesAndAlignmentsMatch)
{
#if !FLOX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (const auto& s : expectedStructs())
  {
    const FloxStructLayout* entry = findStruct(s.name);
    ASSERT_NE(entry, nullptr) << s.name << " has no layout entry";
    EXPECT_EQ(static_cast<std::size_t>(entry->size), s.size) << s.name;
    EXPECT_EQ(static_cast<std::size_t>(entry->alignment), s.alignment) << s.name;
  }
#endif
}

TEST(CapiEventLayout, TableDescribesNoFieldTheStructDoesNotHave)
{
  // A generated table that grew an entry nobody can account for is as bad as
  // a missing one: the Codon side would import a constant pointing at
  // nothing. Padding members are the only unlisted names allowed.
#if !FLOX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (std::size_t i = 0; i < fieldCount(); ++i)
  {
    const FloxFieldLayout& entry = kFloxEventLayout[i];
    if (entry.field_name[0] == '_')
    {
      continue;
    }
    bool known = false;
    for (const auto& f : expectedFields())
    {
      if (std::strcmp(f.structName, entry.struct_name) == 0 &&
          std::strcmp(f.fieldName, entry.field_name) == 0)
      {
        known = true;
        break;
      }
    }
    EXPECT_TRUE(known) << "layout entry " << entry.struct_name << "."
                       << entry.field_name << " matches no field of that struct";
  }
#endif
}

TEST(CapiEventLayout, RejectReasonIsReachable)
{
  // The field the C ABI fills in on every rejection, and the one Codon had no
  // constant for at all.
#if !FLOX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  const FloxFieldLayout* entry = findField("FloxOrderEventData", "reject_reason");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(static_cast<std::size_t>(entry->offset),
            offsetof(FloxOrderEventData, reject_reason));
#endif
}
