/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"

namespace flox
{

enum class SignalType : uint8_t
{
  Market,
  Limit,
  Cancel,
  CancelAll
};

struct Signal
{
  SignalType type{SignalType::Market};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderId orderId{};  // for Cancel

  static Signal marketBuy(SymbolId sym, Quantity qty)
  {
    return Signal{SignalType::Market, sym, Side::BUY, Price{}, qty, 0};
  }

  static Signal marketSell(SymbolId sym, Quantity qty)
  {
    return Signal{SignalType::Market, sym, Side::SELL, Price{}, qty, 0};
  }

  static Signal limitBuy(SymbolId sym, Price px, Quantity qty)
  {
    return Signal{SignalType::Limit, sym, Side::BUY, px, qty, 0};
  }

  static Signal limitSell(SymbolId sym, Price px, Quantity qty)
  {
    return Signal{SignalType::Limit, sym, Side::SELL, px, qty, 0};
  }

  static Signal cancel(OrderId id)
  {
    return Signal{SignalType::Cancel, 0, Side::BUY, Price{}, Quantity{}, id};
  }

  static Signal cancelAll(SymbolId sym)
  {
    return Signal{SignalType::CancelAll, sym, Side::BUY, Price{}, Quantity{}, 0};
  }
};

}  // namespace flox
