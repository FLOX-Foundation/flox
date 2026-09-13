/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// order_type_names.hpp — the one canonical string <-> code table for each
// order-type code space that crosses the C ABI.
//
// Every binding that turns an order-type code into a string, or a string
// into a code, must go through this header instead of keeping its own
// private copy. CAPI-01, CAPI-09 and NC-06 were all downstream of hand-kept
// tables that drifted from each other and from the two spaces below.
//
// Space A — flox::OrderType (include/flox/common.h). Governs
// FloxOrder.type, FloxOrderEventData.order_type, and the order_type
// parameter of flox_simulated_executor_submit_order[_ex]:
//   LIMIT=0, MARKET=1, STOP_MARKET=2, STOP_LIMIT=3, TAKE_PROFIT_MARKET=4,
//   TAKE_PROFIT_LIMIT=5, TRAILING_STOP=6, ICEBERG=7.
//
// Space B — FLOX_SIGNAL_TYPE_* (include/flox/capi/flox_capi.h). Governs
// FloxSignal.order_type only:
//   MARKET=0, LIMIT=1, STOP_MARKET=2, STOP_LIMIT=3, TAKE_PROFIT_MARKET=4,
//   TAKE_PROFIT_LIMIT=5, TRAILING_STOP=6, CANCEL=7, CANCEL_ALL=8, MODIFY=9,
//   ICEBERG=10.
//
// LIMIT and MARKET are swapped between the two spaces on purpose — see
// signalTypeCodeFromOrderType in src/capi/flox_capi.cpp. Never hand a code
// from one space to a function or table that expects the other.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace flox::capi
{

// Space A, lower-case form (e.g. FloxOrder.orderType as seen by JS/Python).
inline constexpr const char* kOrderTypeNamesLower[] = {
    "limit",
    "market",
    "stop_market",
    "stop_limit",
    "tp_market",
    "tp_limit",
    "trailing_stop",
    "iceberg",
};

// Space A, upper-case form (e.g. FloxOrderEventData.orderType as seen by
// JS/Python onFill / onOrderUpdate payloads).
inline constexpr const char* kOrderTypeNamesUpper[] = {
    "LIMIT",
    "MARKET",
    "STOP_MARKET",
    "STOP_LIMIT",
    "TP_MARKET",
    "TP_LIMIT",
    "TRAILING_STOP",
    "ICEBERG",
};

inline constexpr std::size_t kOrderTypeNameCount =
    sizeof(kOrderTypeNamesLower) / sizeof(kOrderTypeNamesLower[0]);

inline const char* orderTypeNameLower(uint8_t code)
{
  return code < kOrderTypeNameCount ? kOrderTypeNamesLower[code] : "unknown";
}

inline const char* orderTypeNameUpper(uint8_t code)
{
  return code < kOrderTypeNameCount ? kOrderTypeNamesUpper[code] : "UNKNOWN";
}

// Encodes a Space A order-type string (lower-case form) into its wire code.
// Returns false and leaves *out untouched for an unrecognized name.
inline bool orderTypeCodeFromName(const std::string& name, uint8_t* out)
{
  for (std::size_t i = 0; i < kOrderTypeNameCount; ++i)
  {
    if (name == kOrderTypeNamesLower[i])
    {
      *out = static_cast<uint8_t>(i);
      return true;
    }
  }
  return false;
}

// Space B (FloxSignal.order_type, e.g. the Signal.orderType JS/Python
// string seen by onSignal / storage-sink / gate hooks).
inline constexpr const char* kSignalTypeNames[] = {
    "market",
    "limit",
    "stop_market",
    "stop_limit",
    "tp_market",
    "tp_limit",
    "trailing_stop",
    "cancel",
    "cancel_all",
    "modify",
    "iceberg",
};

inline constexpr std::size_t kSignalTypeNameCount =
    sizeof(kSignalTypeNames) / sizeof(kSignalTypeNames[0]);

inline const char* signalTypeName(uint8_t code)
{
  return code < kSignalTypeNameCount ? kSignalTypeNames[code] : "unknown";
}

}  // namespace flox::capi
