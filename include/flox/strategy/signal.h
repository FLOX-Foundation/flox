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
  CancelAll,
  Modify
};

struct Signal
{
  SignalType type{SignalType::Market};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderId orderId{};
  Price newPrice{};        // for Modify
  Quantity newQuantity{};  // for Modify

  static Signal marketBuy(SymbolId sym, Quantity qty, OrderId id)
  {
    return Signal{SignalType::Market, sym, Side::BUY, Price{}, qty, id, Price{}, Quantity{}};
  }

  static Signal marketSell(SymbolId sym, Quantity qty, OrderId id)
  {
    return Signal{SignalType::Market, sym, Side::SELL, Price{}, qty, id, Price{}, Quantity{}};
  }

  static Signal limitBuy(SymbolId sym, Price px, Quantity qty, OrderId id)
  {
    return Signal{SignalType::Limit, sym, Side::BUY, px, qty, id, Price{}, Quantity{}};
  }

  static Signal limitSell(SymbolId sym, Price px, Quantity qty, OrderId id)
  {
    return Signal{SignalType::Limit, sym, Side::SELL, px, qty, id, Price{}, Quantity{}};
  }

  static Signal cancel(OrderId id)
  {
    return Signal{SignalType::Cancel, 0, Side::BUY, Price{}, Quantity{}, id, Price{}, Quantity{}};
  }

  static Signal cancelAll(SymbolId sym)
  {
    return Signal{SignalType::CancelAll, sym, Side::BUY, Price{}, Quantity{}, 0, Price{}, Quantity{}};
  }

  static Signal modify(OrderId id, Price newPx, Quantity newQty)
  {
    return Signal{SignalType::Modify, 0, Side::BUY, Price{}, Quantity{}, id, newPx, newQty};
  }
};

}  // namespace flox
