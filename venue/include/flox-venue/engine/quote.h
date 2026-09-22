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

// A ladder, read as the Quotes it is.
//
// Holds nothing either, and adds no second reading of anything: rung i of a
// ladder IS a Quote, built here, and the engine then runs the quote path it
// already had. That is what makes "one ladder equals K Quotes" a property of
// the code rather than a claim a test has to keep checking -- there is only
// one quote path, and the ladder walks it K times.
//
// K, not `levels`: the rungs past the live ones are Quotes with no quantity
// on either side, which is precisely a quote that takes its two legs down and
// posts nothing. That is how a ladder that got shorter removes the levels it
// dropped, with no ladder state anywhere in the venue -- the ids are the
// ladder's own block, so the engine can name a level the submitter stopped
// naming.
class QuoteLadderLegs
{
 public:
  // Rung `i` of `l` as the Quote it is. Every field the ladder carries is a
  // field the quote carries; the ids are slot `i` of the ladder's two id
  // blocks; the prices and sizes come from the rung, or are zero past the
  // live ones.
  static Quote at(const QuoteLadder& l, uint8_t i)
  {
    Quote q;
    q.bidId = l.bidIdBase + i;
    q.askId = l.askIdBase + i;
    q.symbol = l.symbol;
    if (i < quoteLadderLiveLevels(l))
    {
      q.bidPrice = l.level[i].bidPrice;
      q.bidQty = l.level[i].bidQty;
      q.askPrice = l.level[i].askPrice;
      q.askQty = l.level[i].askQty;
    }
    q.accountId = l.accountId;
    q.stp = l.stp;
    q.lastLook = l.lastLook;
    q.postOnly = l.postOnly;
    q.reduceOnly = l.reduceOnly;
    q.tif = l.tif;
    q.visibleQuantity = l.visibleQuantity;
    q.expiryNs = l.expiryNs;
    q.clientOrderId = l.clientOrderId;
    return q;
  }
};

}  // namespace flox::venue::engine
