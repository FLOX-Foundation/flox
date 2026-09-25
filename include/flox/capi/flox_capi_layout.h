/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * GENERATED — do not edit by hand.
 * Source: include/flox/capi/flox_capi_spec.hpp
 * Tool:   tools/codegen/flox_codegen/emit_layout.py
 *
 * Byte layout of the event structs a binding may read through raw memory.
 * codon/flox/layout.codon is generated from the same pass; the offsets there
 * are these offsets. tests/test_capi_event_layout.cpp compares every entry
 * below against offsetof/sizeof on the real struct, so a layout the compiler
 * disagrees with fails the build instead of a user's strategy.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct
  {
    const char* struct_name;
    const char* field_name;
    size_t offset;
    size_t size;
  } FloxFieldLayout;

  typedef struct
  {
    const char* struct_name;
    size_t size;
    size_t alignment;
  } FloxStructLayout;

  static const FloxFieldLayout kFloxEventLayout[] = {
      {"FloxTradeData", "symbol", 0, 4},
      {"FloxTradeData", "price_raw", 8, 8},
      {"FloxTradeData", "quantity_raw", 16, 8},
      {"FloxTradeData", "is_buy", 24, 1},
      {"FloxTradeData", "exchange_ts_ns", 32, 8},
      {"FloxBookSnapshot", "bid_price_raw", 0, 8},
      {"FloxBookSnapshot", "bid_qty_raw", 8, 8},
      {"FloxBookSnapshot", "ask_price_raw", 16, 8},
      {"FloxBookSnapshot", "ask_qty_raw", 24, 8},
      {"FloxBookSnapshot", "mid_raw", 32, 8},
      {"FloxBookSnapshot", "spread_raw", 40, 8},
      {"FloxSymbolContext", "symbol_id", 0, 4},
      {"FloxSymbolContext", "has_avg_entry_price", 4, 1},
      {"FloxSymbolContext", "position_raw", 8, 8},
      {"FloxSymbolContext", "avg_entry_price_raw", 16, 8},
      {"FloxSymbolContext", "last_trade_price_raw", 24, 8},
      {"FloxSymbolContext", "last_update_ns", 32, 8},
      {"FloxSymbolContext", "book", 40, 48},
      {"FloxBarData", "symbol", 0, 4},
      {"FloxBarData", "bar_type", 4, 1},
      {"FloxBarData", "close_reason", 5, 1},
      {"FloxBarData", "bar_type_param", 8, 8},
      {"FloxBarData", "open_raw", 16, 8},
      {"FloxBarData", "high_raw", 24, 8},
      {"FloxBarData", "low_raw", 32, 8},
      {"FloxBarData", "close_raw", 40, 8},
      {"FloxBarData", "volume_raw", 48, 8},
      {"FloxBarData", "buy_volume_raw", 56, 8},
      {"FloxBarData", "trade_count_raw", 64, 8},
      {"FloxBarData", "start_time_ns", 72, 8},
      {"FloxBarData", "end_time_ns", 80, 8},
      {"FloxOrderEventData", "order_id", 0, 8},
      {"FloxOrderEventData", "symbol_id", 8, 4},
      {"FloxOrderEventData", "side", 12, 1},
      {"FloxOrderEventData", "order_type", 13, 1},
      {"FloxOrderEventData", "status", 14, 1},
      {"FloxOrderEventData", "fill_qty_raw", 16, 8},
      {"FloxOrderEventData", "fill_price_raw", 24, 8},
      {"FloxOrderEventData", "exchange_ts_ns", 32, 8},
      {"FloxOrderEventData", "reject_reason", 40, 8},
      {"FloxOrderEventData", "queue_ahead_raw", 48, 8},
      {"FloxOrderEventData", "queue_total_raw", 56, 8},
      {"FloxOrderEventData", "submitted_at_ns", 64, 8},
      {"FloxOrderEventData", "accepted_at_ns", 72, 8},
      {"FloxOrderEventData", "first_fill_at_ns", 80, 8},
      {"FloxOrderEventData", "last_fill_at_ns", 88, 8},
      {"FloxOrderEventData", "canceled_at_ns", 96, 8},
      {"FloxOrderEventData", "rejected_at_ns", 104, 8},
      {"FloxOrderEventData", "triggered_at_ns", 112, 8},
      {"FloxOrderEventData", "expired_at_ns", 120, 8},
      {"FloxOrderEventData", "is_maker", 128, 1},
      {"FloxOrderEventData", "market_position", 129, 1},
      {"FloxOrderEventData", "distance_to_best_ticks", 132, 4},
      {"FloxBar", "start_time_ns", 0, 8},
      {"FloxBar", "end_time_ns", 8, 8},
      {"FloxBar", "open_raw", 16, 8},
      {"FloxBar", "high_raw", 24, 8},
      {"FloxBar", "low_raw", 32, 8},
      {"FloxBar", "close_raw", 40, 8},
      {"FloxBar", "volume_raw", 48, 8},
      {"FloxBar", "buy_volume_raw", 56, 8},
      {"FloxBar", "trade_count", 64, 4},
      {"FloxBar", "close_reason", 68, 1},
  };

  static const FloxStructLayout kFloxEventStructLayout[] = {
      {"FloxTradeData", 40, 8},
      {"FloxBookSnapshot", 48, 8},
      {"FloxSymbolContext", 88, 8},
      {"FloxBarData", 88, 8},
      {"FloxOrderEventData", 144, 8},
      {"FloxBar", 72, 8},
  };

#ifdef __cplusplus
}
#endif
