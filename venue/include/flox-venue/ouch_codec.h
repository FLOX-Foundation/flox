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
#include "flox/util/wire.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace flox::venue
{

// Inbound message tags (client -> venue).
enum class OuchIn : uint8_t
{
  EnterOrder = 'O',
  CancelOrder = 'X',
  ReplaceOrder = 'U',
};

// Outbound message tags (venue -> client).
enum class OuchOut : uint8_t
{
  Accepted = 'A',
  Executed = 'E',
  Trade = 'T',
  Canceled = 'C',
  Rejected = 'J',
  Replaced = 'R',
  Triggered = 'G',
};

class OuchCodec
{
 public:
  // ---- inbound: wire -> InboundCommand ----
  static void encode(const InboundCommand& cmd, std::vector<uint8_t>& out)
  {
    out.clear();
    if (const auto* n = std::get_if<NewOrder>(&cmd))
    {
      out.push_back(static_cast<uint8_t>(OuchIn::EnterOrder));
      wire::put(out, n->id, 8);
      wire::put(out, n->symbol, 4);
      out.push_back(static_cast<uint8_t>(n->side));
      out.push_back(static_cast<uint8_t>(n->type));
      out.push_back(static_cast<uint8_t>(n->tif));
      out.push_back(n->postOnly ? 1 : 0);
      out.push_back(static_cast<uint8_t>(n->stp));
      wire::put(out, static_cast<uint64_t>(n->price.raw()), 8);
      wire::put(out, static_cast<uint64_t>(n->quantity.raw()), 8);
      wire::put(out, static_cast<uint64_t>(n->visibleQuantity.raw()), 8);
      wire::put(out, static_cast<uint64_t>(n->triggerPrice.raw()), 8);
      wire::put(out, static_cast<uint64_t>(n->trailingOffset.raw()), 8);
      wire::put(out, n->accountId, 8);
      wire::put(out, n->clientOrderId, 8);
      // Derivatives / MM / advanced-order fields (must not be silently dropped:
      // a reduce-only order that loses the flag would OPEN a position).
      out.push_back(n->reduceOnly ? 1 : 0);
      out.push_back(n->lastLook ? 1 : 0);
      out.push_back(static_cast<uint8_t>(n->peg));
      wire::put(out, static_cast<uint64_t>(n->expiryNs), 8);
      wire::put(out, n->ocoGroup, 8);
      wire::put(out, static_cast<uint64_t>(n->pegOffsetRaw), 8);
    }
    else if (const auto* c = std::get_if<CancelOrder>(&cmd))
    {
      out.push_back(static_cast<uint8_t>(OuchIn::CancelOrder));
      wire::put(out, c->id, 8);
      wire::put(out, c->symbol, 4);
      wire::put(out, c->accountId, 8);
    }
    else
    {
      const auto& m = std::get<ModifyOrder>(cmd);
      out.push_back(static_cast<uint8_t>(OuchIn::ReplaceOrder));
      wire::put(out, m.id, 8);
      wire::put(out, m.symbol, 4);
      wire::put(out, static_cast<uint64_t>(m.newPrice.raw()), 8);
      wire::put(out, static_cast<uint64_t>(m.newQty.raw()), 8);
      wire::put(out, m.accountId, 8);
    }
  }

  static std::optional<InboundCommand> decode(const uint8_t* p, size_t n)
  {
    wire::Reader r{p, n, 0, true};
    const uint8_t tag = r.u8();
    if (tag == static_cast<uint8_t>(OuchIn::EnterOrder))
    {
      NewOrder o;
      o.id = r.u(8);
      o.symbol = static_cast<SymbolId>(r.u(4));
      o.side = static_cast<Side>(r.u8());
      o.type = static_cast<OrderType>(r.u8());
      o.tif = static_cast<TimeInForce>(r.u8());
      o.postOnly = r.u8() != 0;
      o.stp = static_cast<STPMode>(r.u8());
      o.price = Price::fromRaw(r.i(8));
      o.quantity = Quantity::fromRaw(r.i(8));
      o.visibleQuantity = Quantity::fromRaw(r.i(8));
      o.triggerPrice = Price::fromRaw(r.i(8));
      o.trailingOffset = Price::fromRaw(r.i(8));
      o.accountId = r.u(8);
      o.clientOrderId = r.u(8);
      o.reduceOnly = r.u8() != 0;
      o.lastLook = r.u8() != 0;
      o.peg = static_cast<PegRef>(r.u8());
      o.expiryNs = static_cast<int64_t>(r.u(8));
      o.ocoGroup = r.u(8);
      o.pegOffsetRaw = static_cast<int64_t>(r.u(8));
      return r.ok ? std::optional<InboundCommand>{o} : std::nullopt;
    }
    if (tag == static_cast<uint8_t>(OuchIn::CancelOrder))
    {
      CancelOrder c;
      c.id = r.u(8);
      c.symbol = static_cast<SymbolId>(r.u(4));
      c.accountId = r.u(8);
      return r.ok ? std::optional<InboundCommand>{c} : std::nullopt;
    }
    if (tag == static_cast<uint8_t>(OuchIn::ReplaceOrder))
    {
      ModifyOrder m;
      m.id = r.u(8);
      m.symbol = static_cast<SymbolId>(r.u(4));
      m.newPrice = Price::fromRaw(r.i(8));
      m.newQty = Quantity::fromRaw(r.i(8));
      m.accountId = r.u(8);
      return r.ok ? std::optional<InboundCommand>{m} : std::nullopt;
    }
    return std::nullopt;
  }

  // ---- outbound: OutboundEvent -> wire exec report ----
  static void encode(const OutboundEvent& ev, std::vector<uint8_t>& out)
  {
    out.clear();
    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      out.push_back(static_cast<uint8_t>(OuchOut::Accepted));
      wire::put(out, a->id, 8);
      wire::put(out, a->symbol, 4);
      out.push_back(static_cast<uint8_t>(a->side));
      wire::put(out, static_cast<uint64_t>(a->price.raw()), 8);
      wire::put(out, static_cast<uint64_t>(a->leavesQty.raw()), 8);
      out.push_back(a->restingOnBook ? 1 : 0);
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&ev))
    {
      out.push_back(static_cast<uint8_t>(OuchOut::Executed));
      wire::put(out, x->id, 8);
      wire::put(out, x->symbol, 4);
      wire::put(out, static_cast<uint64_t>(x->lastQty.raw()), 8);
      wire::put(out, static_cast<uint64_t>(x->lastPx.raw()), 8);  // fill price
      wire::put(out, static_cast<uint64_t>(x->leavesQty.raw()), 8);
      out.push_back(x->aggressor ? 1 : 0);
      out.push_back(x->complete ? 1 : 0);
    }
    else if (const auto* t = std::get_if<Trade>(&ev))
    {
      out.push_back(static_cast<uint8_t>(OuchOut::Trade));
      wire::put(out, t->tradeId, 8);
      wire::put(out, t->symbol, 4);
      wire::put(out, static_cast<uint64_t>(t->price.raw()), 8);
      wire::put(out, static_cast<uint64_t>(t->quantity.raw()), 8);
      wire::put(out, t->makerId, 8);
      wire::put(out, t->takerId, 8);
      out.push_back(static_cast<uint8_t>(t->takerSide));
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      out.push_back(static_cast<uint8_t>(OuchOut::Canceled));
      wire::put(out, c->id, 8);
      wire::put(out, c->symbol, 4);
      out.push_back(static_cast<uint8_t>(c->reason));
    }
    else if (const auto* j = std::get_if<OrderRejected>(&ev))
    {
      out.push_back(static_cast<uint8_t>(OuchOut::Rejected));
      wire::put(out, j->id, 8);
      wire::put(out, j->symbol, 4);
      out.push_back(static_cast<uint8_t>(j->reason));
    }
    else if (const auto* m = std::get_if<OrderModified>(&ev))
    {
      out.push_back(static_cast<uint8_t>(OuchOut::Replaced));
      wire::put(out, m->id, 8);
      wire::put(out, m->symbol, 4);
      wire::put(out, static_cast<uint64_t>(m->price.raw()), 8);
      wire::put(out, static_cast<uint64_t>(m->leavesQty.raw()), 8);
      out.push_back(m->priorityKept ? 1 : 0);
    }
    else if (const auto* g = std::get_if<OrderTriggered>(&ev))
    {
      out.push_back(static_cast<uint8_t>(OuchOut::Triggered));
      wire::put(out, g->id, 8);
      wire::put(out, g->symbol, 4);
      wire::put(out, static_cast<uint64_t>(g->refPrice.raw()), 8);
    }
  }
};

}  // namespace flox::venue
