/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include <array>
#include <cstdint>

namespace flox::venue::engine
{

// A two-sided quote, read as the child orders it names.
//
// The component holds NOTHING: a quote is one command that becomes up to two
// orders and is then finished with -- there is no quote state anywhere in the
// venue, and a maker's "current quote" is simply the two orders resting under
// the ids it keeps naming. What is here is the reading: which legs the quote
// asks for, and what each of them carries.
//
// Every field a quote carries is a field its legs carry. That is the whole
// rule, and it used to be written out twice, once per side, which is how a
// field gets added to a quote and reaches only its bid. Written once, over a
// side, it cannot.
//
// The clientOrderId is the name the submitter gave the QUOTE, not a per-leg
// id it never chose: both legs carry it so their reports can be told apart
// from any other order and joined back to each other. Its dedup slot is
// consumed once, for the quote as a whole, before either leg is built -- that
// is the engine's call and it is made before this is asked anything.
class QuoteLegs
{
 public:
  // The legs `q` asks for, written into `out` and counted by the return: bid
  // first, ask second, and a side quoted at zero omitted (quoting nothing on
  // a side means leaving it empty, which the cancel of the prior leg already
  // did). So the count is 0, 1 or 2 and the order is the order the engine
  // submits them in -- the bid of a two-sided quote reaches the book first.
  static uint8_t build(const Quote& q, SymbolId symbol, std::array<NewOrder, 2>& out)
  {
    uint8_t n = 0;
    if (q.bidQty.raw() > 0)
    {
      out[n++] = leg(q, symbol, Side::BUY);
    }
    if (q.askQty.raw() > 0)
    {
      out[n++] = leg(q, symbol, Side::SELL);
    }
    return n;
  }

  // One side of `q` as the order it becomes. A quote leg is always a LIMIT:
  // the price it names is a price, not a trigger and not a sweep.
  static NewOrder leg(const Quote& q, SymbolId symbol, Side side)
  {
    const bool buy = (side == Side::BUY);
    NewOrder o;
    o.id = buy ? q.bidId : q.askId;
    o.symbol = symbol;
    o.side = side;
    o.type = OrderType::LIMIT;
    o.price = buy ? q.bidPrice : q.askPrice;
    o.quantity = buy ? q.bidQty : q.askQty;
    o.accountId = q.accountId;
    o.stp = q.stp;
    o.lastLook = q.lastLook;
    o.postOnly = q.postOnly;
    o.reduceOnly = q.reduceOnly;
    o.tif = q.tif;
    o.visibleQuantity = q.visibleQuantity;
    o.expiryNs = q.expiryNs;
    o.clientOrderId = q.clientOrderId;
    return o;
  }
};

}  // namespace flox::venue::engine
