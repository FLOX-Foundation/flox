/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// python/order_type_bindings.h
//
// The Python binding's single door onto the canonical order-type table in
// include/flox/capi/order_type_names.hpp.
//
// It used to keep three hand-written copies instead -- one in
// strategy_bindings.h, one in backtest_bindings.h, one in
// bare_executor_bindings.h -- and they drifted from the canonical table
// and from each other: two of them spelled codes 4 and 5
// "take_profit_market"/"take_profit_limit" where the table says
// "tp_market"/"tp_limit", the third had no entry for ICEBERG, and both
// parsers answered an unrecognised name with OrderType::MARKET. Handing
// them the canonical "tp_market" therefore submitted a market order, and
// so did a typo.
//
// Everything here goes through the canonical table, and a name it does
// not know is a ValueError naming the accepted set, never a market
// order.

#pragma once

#include <pybind11/pybind11.h>

#include "flox/capi/order_type_names.hpp"
#include "flox/common.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace flox_py
{

// Raises ValueError in Python for a name outside the canonical table.
inline flox::OrderType parseOrderTypeStrict(const std::string& name)
{
  uint8_t code = 0;
  if (!flox::capi::orderTypeCodeFromName(name, &code))
  {
    throw std::invalid_argument(flox::capi::unknownOrderTypeMessage(name));
  }
  return static_cast<flox::OrderType>(code);
}

inline void bindOrderTypes(py::module_& m)
{
  py::tuple names(flox::capi::kOrderTypeNameCount);
  for (std::size_t i = 0; i < flox::capi::kOrderTypeNameCount; ++i)
  {
    names[i] = flox::capi::kOrderTypeNamesLower[i];
  }
  m.attr("ORDER_TYPE_NAMES") = names;

  m.def(
      "order_type_name",
      [](uint32_t code)
      {
        if (code >= flox::capi::kOrderTypeNameCount)
        {
          throw std::invalid_argument(
              "order type code " + std::to_string(code) + " is outside the table; codes 0.." +
              std::to_string(flox::capi::kOrderTypeNameCount - 1) + " name " +
              flox::capi::orderTypeNameList() + ".");
        }
        return std::string(flox::capi::kOrderTypeNamesLower[code]);
      },
      "Canonical name of an order-type wire code (flox::OrderType space).\n"
      "Raises ValueError for a code outside ORDER_TYPE_NAMES.",
      py::arg("code"));

  m.def(
      "order_type_code",
      [](const std::string& name)
      {
        uint8_t code = 0;
        if (!flox::capi::orderTypeCodeFromName(name, &code))
        {
          throw std::invalid_argument(flox::capi::unknownOrderTypeMessage(name));
        }
        return static_cast<uint32_t>(code);
      },
      "Wire code of a canonical order-type name (flox::OrderType space).\n"
      "Raises ValueError naming the accepted set for anything else.",
      py::arg("name"));
}

}  // namespace flox_py
