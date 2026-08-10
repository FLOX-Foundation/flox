/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * SBE (Simple Binary Encoding) codec for venue order entry + execution reports.
 *
 * Source of truth: venue/schema/order-entry-sbe.xml. Hand-written against that
 * schema (no Java codegen in a C++/Python build). Same SBE contract as the
 * market-data codec: a canonical message header (flox-venue/sbe.h) precedes a
 * fixed little-endian root block; the header's blockLength lets a decoder skip
 * trailing fields a newer version appended; a foreign schemaId is rejected.
 * One template per message type, carrying only that type's fields.
 *
 * Direction is by templateId: inbound (client -> venue) EnterOrder/Cancel/
 * Replace use ids 1..3; outbound execution reports use ids 10..16. decode()
 * handles the inbound side; encode() serialises either an InboundCommand or an
 * OutboundEvent.
 *
 * This is the venue's own SBE order-entry schema -- what real crypto venues use
 * for binary order entry (Deribit, Binance). It replaced a homegrown binary
 * record that was misleadingly named "OUCH".
 */
#pragma once

#include "flox-venue/messages.h"
#include "flox-venue/sbe.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace flox::venue
{

class SbeOrderEntryCodec
{
 public:
  static constexpr uint16_t kSchemaId = 92;
  static constexpr uint16_t kVersion = 0;

  enum class InTmpl : uint16_t
  {
    EnterOrder = 1,
    CancelOrder = 2,
    ReplaceOrder = 3,
  };
  enum class OutTmpl : uint16_t
  {
    Accepted = 10,
    Executed = 11,
    Trade = 12,
    Canceled = 13,
    Rejected = 14,
    Replaced = 15,
    Triggered = 16,
  };

  // Root-block lengths (bytes after the header). Must match order-entry-sbe.xml.
  static constexpr uint16_t kBlockEnter = 100;
  static constexpr uint16_t kBlockCancel = 20;
  static constexpr uint16_t kBlockReplace = 36;
  static constexpr uint16_t kBlockAccepted = 30;
  static constexpr uint16_t kBlockExecuted = 38;
  static constexpr uint16_t kBlockTrade = 45;
  static constexpr uint16_t kBlockCanceled = 13;
  static constexpr uint16_t kBlockRejected = 13;
  static constexpr uint16_t kBlockReplaced = 29;
  static constexpr uint16_t kBlockTriggered = 20;

  static constexpr size_t kMaxSize = sbe::kHeaderSize + kBlockEnter;

  // Read the templateId of a framed message (0 if too short / foreign schema).
  // Lets a consumer classify an exec report without a full decode.
  static uint16_t templateId(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return 0;
    }
    const sbe::Header h = sbe::readHeader(p);
    return h.schemaId == kSchemaId ? h.templateId : 0;
  }

  // ---- inbound: client command -> wire ----
  static void encode(const InboundCommand& cmd, std::vector<uint8_t>& out)
  {
    out.clear();
    out.reserve(kMaxSize);
    if (const auto* n = std::get_if<NewOrder>(&cmd))
    {
      sbe::putHeader(out, kBlockEnter, u16(InTmpl::EnterOrder), kSchemaId, kVersion);
      sbe::putU64(out, n->id);
      sbe::putU32(out, n->symbol);
      sbe::putU8(out, static_cast<uint8_t>(n->side));
      sbe::putU8(out, static_cast<uint8_t>(n->type));
      sbe::putU8(out, static_cast<uint8_t>(n->tif));
      sbe::putU8(out, n->postOnly ? 1 : 0);
      sbe::putU8(out, static_cast<uint8_t>(n->stp));
      sbe::putI64(out, n->price.raw());
      sbe::putI64(out, n->quantity.raw());
      sbe::putI64(out, n->visibleQuantity.raw());
      sbe::putI64(out, n->triggerPrice.raw());
      sbe::putI64(out, n->trailingOffset.raw());
      sbe::putU64(out, n->accountId);
      sbe::putU64(out, n->clientOrderId);
      sbe::putU8(out, n->reduceOnly ? 1 : 0);
      sbe::putU8(out, n->lastLook ? 1 : 0);
      sbe::putU8(out, static_cast<uint8_t>(n->peg));
      sbe::putI64(out, n->expiryNs);
      sbe::putU64(out, n->ocoGroup);
      sbe::putI64(out, n->pegOffsetRaw);
    }
    else if (const auto* c = std::get_if<CancelOrder>(&cmd))
    {
      sbe::putHeader(out, kBlockCancel, u16(InTmpl::CancelOrder), kSchemaId, kVersion);
      sbe::putU64(out, c->id);
      sbe::putU32(out, c->symbol);
      sbe::putU64(out, c->accountId);
    }
    else if (const auto* m = std::get_if<ModifyOrder>(&cmd))
    {
      sbe::putHeader(out, kBlockReplace, u16(InTmpl::ReplaceOrder), kSchemaId, kVersion);
      sbe::putU64(out, m->id);
      sbe::putU32(out, m->symbol);
      sbe::putI64(out, m->newPrice.raw());
      sbe::putI64(out, m->newQty.raw());
      sbe::putU64(out, m->accountId);
    }
    // Other InboundCommand alternatives are not part of the order-entry wire.
  }

  static std::optional<InboundCommand> decode(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return std::nullopt;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId)
    {
      return std::nullopt;
    }
    if (n < sbe::kHeaderSize + h.blockLength)
    {
      return std::nullopt;  // truncated root block
    }
    const uint8_t* b = p + sbe::kHeaderSize;

    switch (static_cast<InTmpl>(h.templateId))
    {
      case InTmpl::EnterOrder:
      {
        if (h.blockLength < kBlockEnter)
        {
          return std::nullopt;
        }
        NewOrder o;
        o.id = sbe::getU64(b + 0);
        o.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
        o.side = static_cast<Side>(b[12]);
        o.type = static_cast<OrderType>(b[13]);
        o.tif = static_cast<TimeInForce>(b[14]);
        o.postOnly = b[15] != 0;
        o.stp = static_cast<STPMode>(b[16]);
        o.price = Price::fromRaw(sbe::getI64(b + 17));
        o.quantity = Quantity::fromRaw(sbe::getI64(b + 25));
        o.visibleQuantity = Quantity::fromRaw(sbe::getI64(b + 33));
        o.triggerPrice = Price::fromRaw(sbe::getI64(b + 41));
        o.trailingOffset = Price::fromRaw(sbe::getI64(b + 49));
        o.accountId = sbe::getU64(b + 57);
        o.clientOrderId = sbe::getU64(b + 65);
        o.reduceOnly = b[73] != 0;
        o.lastLook = b[74] != 0;
        o.peg = static_cast<PegRef>(b[75]);
        o.expiryNs = sbe::getI64(b + 76);
        o.ocoGroup = sbe::getU64(b + 84);
        o.pegOffsetRaw = sbe::getI64(b + 92);
        return InboundCommand{o};
      }
      case InTmpl::CancelOrder:
      {
        if (h.blockLength < kBlockCancel)
        {
          return std::nullopt;
        }
        CancelOrder c;
        c.id = sbe::getU64(b + 0);
        c.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
        c.accountId = sbe::getU64(b + 12);
        return InboundCommand{c};
      }
      case InTmpl::ReplaceOrder:
      {
        if (h.blockLength < kBlockReplace)
        {
          return std::nullopt;
        }
        ModifyOrder m;
        m.id = sbe::getU64(b + 0);
        m.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
        m.newPrice = Price::fromRaw(sbe::getI64(b + 12));
        m.newQty = Quantity::fromRaw(sbe::getI64(b + 20));
        m.accountId = sbe::getU64(b + 28);
        return InboundCommand{m};
      }
    }
    return std::nullopt;  // unknown / outbound template
  }

  // ---- outbound: execution report -> wire ----
  static void encode(const OutboundEvent& ev, std::vector<uint8_t>& out)
  {
    out.clear();
    out.reserve(kMaxSize);
    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      sbe::putHeader(out, kBlockAccepted, u16(OutTmpl::Accepted), kSchemaId, kVersion);
      sbe::putU64(out, a->id);
      sbe::putU32(out, a->symbol);
      sbe::putU8(out, static_cast<uint8_t>(a->side));
      sbe::putI64(out, a->price.raw());
      sbe::putI64(out, a->leavesQty.raw());
      sbe::putU8(out, a->restingOnBook ? 1 : 0);
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&ev))
    {
      sbe::putHeader(out, kBlockExecuted, u16(OutTmpl::Executed), kSchemaId, kVersion);
      sbe::putU64(out, x->id);
      sbe::putU32(out, x->symbol);
      sbe::putI64(out, x->lastQty.raw());
      sbe::putI64(out, x->lastPx.raw());
      sbe::putI64(out, x->leavesQty.raw());
      sbe::putU8(out, x->aggressor ? 1 : 0);
      sbe::putU8(out, x->complete ? 1 : 0);
    }
    else if (const auto* t = std::get_if<Trade>(&ev))
    {
      sbe::putHeader(out, kBlockTrade, u16(OutTmpl::Trade), kSchemaId, kVersion);
      sbe::putU64(out, t->tradeId);
      sbe::putU32(out, t->symbol);
      sbe::putI64(out, t->price.raw());
      sbe::putI64(out, t->quantity.raw());
      sbe::putU64(out, t->makerId);
      sbe::putU64(out, t->takerId);
      sbe::putU8(out, static_cast<uint8_t>(t->takerSide));
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      sbe::putHeader(out, kBlockCanceled, u16(OutTmpl::Canceled), kSchemaId, kVersion);
      sbe::putU64(out, c->id);
      sbe::putU32(out, c->symbol);
      sbe::putU8(out, static_cast<uint8_t>(c->reason));
    }
    else if (const auto* j = std::get_if<OrderRejected>(&ev))
    {
      sbe::putHeader(out, kBlockRejected, u16(OutTmpl::Rejected), kSchemaId, kVersion);
      sbe::putU64(out, j->id);
      sbe::putU32(out, j->symbol);
      sbe::putU8(out, static_cast<uint8_t>(j->reason));
    }
    else if (const auto* m = std::get_if<OrderModified>(&ev))
    {
      sbe::putHeader(out, kBlockReplaced, u16(OutTmpl::Replaced), kSchemaId, kVersion);
      sbe::putU64(out, m->id);
      sbe::putU32(out, m->symbol);
      sbe::putI64(out, m->price.raw());
      sbe::putI64(out, m->leavesQty.raw());
      sbe::putU8(out, m->priorityKept ? 1 : 0);
    }
    else if (const auto* g = std::get_if<OrderTriggered>(&ev))
    {
      sbe::putHeader(out, kBlockTriggered, u16(OutTmpl::Triggered), kSchemaId, kVersion);
      sbe::putU64(out, g->id);
      sbe::putU32(out, g->symbol);
      sbe::putI64(out, g->refPrice.raw());
    }
    // Events without an order-entry exec-report mapping (FillHeld, FillRejected,
    // MmpTriggered, FeeCharged, Liquidation) produce no frame.
  }

 private:
  static uint16_t u16(InTmpl t) { return static_cast<uint16_t>(t); }
  static uint16_t u16(OutTmpl t) { return static_cast<uint16_t>(t); }
};

}  // namespace flox::venue
