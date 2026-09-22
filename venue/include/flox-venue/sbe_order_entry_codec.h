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
 * Replace use ids 1..3; outbound execution reports use ids 10..18. decode()
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
  // Schema version 1: every OUTBOUND template gained a trailing `seq` (u64,
  // sinceVersion=1) -- the per-session monotonic exec-report sequence number.
  // Appending to the end of the root block (and bumping blockLength) is the
  // SBE-sanctioned extension path: a version-0 reader skips the trailing bytes
  // via the header's blockLength, and this version-1 decoder accepts version-0
  // frames (their shorter blockLength simply has no seq -> seqOf returns 0).
  // Inbound templates are unchanged.
  // Schema version 2:
  //  - SnapshotEnd gained a trailing `lastSeq` (u64, sinceVersion=2): the
  //    stream's last assigned outbound seq at snapshot time, so the client
  //    resumes gap detection from the exact point (a version-1 frame's shorter
  //    blockLength decodes with lastSeq 0).
  //  - New inbound SetSessionConfig (6): wire negotiation of per-session
  //    cancel-on-disconnect (fire-and-forget, handled at the gateway).
  //  - New outbound BalanceUpdate (21): balance change on a sequenced
  //    Deposit/Withdraw (including a rejected withdraw).
  // Schema version 4:
  //  - The five templates that report on an ORDER (Accepted, Executed,
  //    Canceled, Rejected, Replaced) gained a trailing `clOrdId` (u64,
  //    sinceVersion=4): the identifier the submitter gave the order, 0 when it
  //    gave none. Appended after `seq`, so an older reader skips it via
  //    blockLength exactly as it does for seq.
  // Schema version 6:
  //  - FillHeld gained a trailing `takerSide` (EnumU8, sinceVersion=6): the
  //    side of the aggressor whose hit is being held. A client had to rebuild
  //    it from the maker's own accept report; the engine has had it all along.
  //    Appended after `seq` (a version-5 frame's shorter blockLength simply
  //    has no side), which is why seqOffsetIn now knows about this template
  //    too -- `seq` stopped being the last field of the FillHeld block.
  // Schema version 7:
  //  - FillHeld and FillRejected gained a trailing `clOrdId` (u64,
  //    sinceVersion=7): the identifier the TAKER gave its order, 0 when it
  //    gave none. A hold's own id names the hold, not the order that caused
  //    it, so a submitter with a split order had to keep a 37->11 map of its
  //    own to recognise a hold on one of the children. Appended after `seq`
  //    on FillRejected and after `takerSide` on FillHeld (a version-6 frame's
  //    shorter blockLength simply has no id), so seqOffsetIn now knows about
  //    both templates too.
  // 8. Inbound QuoteLadder (template 7): a maker's whole set of levels on one
  //    symbol, replacing the prior set, where the wire previously carried one
  //    replace per level. The rungs are a FIXED block of kQuoteLadderLevels
  //    slots with a count, not an SBE repeating group: this codec has no
  //    var-length support and one message is not a reason to invent it. Slots
  //    past the count are zero-sized rungs, which is how a shorter ladder
  //    takes down the levels it stopped naming.
  // Schema version 9 (T058):
  //  - Canceled gained trailing `leavesQty` and `cumQty` (i64 each,
  //    sinceVersion=9): the residual this cancel actually killed, and what
  //    the order filled before it. A reader that took LeavesQty off a
  //    terminal report (routine for an IOC/FOK residual) previously had no
  //    way to distinguish a full fill from a silently-canceled remainder.
  //  - Rejected gained a trailing `cumQty` (i64, sinceVersion=9), no
  //    leavesQty: a rejected order is never left resting, so that is always
  //    0. cumQty is 0 at every call site this engine has today (an
  //    OrderRejected only ever fires pre-trade), carried for FIX-spec
  //    completeness and so a future reject path following a partial fill
  //    does not misreport by construction.
  //    Both appended after `clOrdId` (a version-8 frame's shorter
  //    blockLength simply has neither), but this DOES move `seq`: unlike
  //    every earlier addition, which landed on templates where clOrdId (or
  //    takerSide) was already the last field, Canceled/Rejected keep
  //    growing past it -- seqOffsetIn has its own version>=9 branch for
  //    these two templates so it keeps subtracting the right trailing width.
  static constexpr uint16_t kVersion = 9;

  enum class InTmpl : uint16_t
  {
    EnterOrder = 1,
    CancelOrder = 2,
    ReplaceOrder = 3,
    // Session-layer verbs (handled by the gateway delivery layer, never
    // forwarded to matching):
    ResendRequest = 4,           // {fromSeq}: replay exec reports with seq >= fromSeq
    AccountSnapshotRequest = 5,  // {}: open orders + position for the session account
    SetSessionConfig = 6,        // {codEnabled}: per-session cancel-on-disconnect
    // A maker's whole set of levels on one symbol, replacing the prior set.
    // Not a session verb: this one reaches matching, as the QuoteLadder
    // command it decodes to.
    QuoteLadder = 7,
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
    FillHeld = 17,      // last-look: fill held pending the maker's decision
    FillRejected = 18,  // last-look: held fill rejected / timed out
    // Session-layer replies (not sequenced, not in the resend log):
    SnapshotRequired = 19,  // resend fromSeq is older than the retained log -> re-sync via snapshot
    SnapshotEnd = 20,       // terminates a snapshot reply (position + open-order count + lastSeq)
    BalanceUpdate = 21,     // balance change on Deposit/Withdraw (sequenced exec-report stream)
    // A refused cancel/replace. FIX answers this with 35=9 rather than an
    // execution report, because an exec report describes the state of an ORDER
    // and a refused cancel changed no order state at all. Same distinction
    // here: its own template, not a variant of Rejected.
    CancelRejected = 22,
    // An operator corrected the account's position. Its own template because
    // it is not an order event at all: no order id, no price, no fee. A client
    // that reconciles against the venue would otherwise see its position jump
    // with nothing on the session to explain it.
    PositionAdjusted = 23,
  };

  // Root-block lengths (bytes after the header). Must match order-entry-sbe.xml.
  // Outbound blocks are the version-1 lengths: version-0 length + 8 (seq).
  static constexpr uint16_t kBlockEnter = 100;
  static constexpr uint16_t kBlockCancel = 20;
  static constexpr uint16_t kBlockReplace = 36;
  static constexpr uint16_t kBlockResendRequest = 8;
  static constexpr uint16_t kBlockSnapshotRequest = 0;
  static constexpr uint16_t kBlockSetSessionConfig = 1;
  // account(8) + clOrdId(8) + expiryNs(8) + visibleQty(8) + bidIdBase(8)
  // + askIdBase(8) + symbol(4) + levels(1) + stp(1) + lastLook(1)
  // + postOnly(1) + reduceOnly(1) + tif(1), then kQuoteLadderLevels rungs of
  // bidPrice/bidQty/askPrice/askQty.
  static constexpr uint16_t kLadderHead = 58;
  static constexpr uint16_t kLadderRung = 32;
  static constexpr uint16_t kBlockQuoteLadder =
      kLadderHead + static_cast<uint16_t>(kQuoteLadderLevels) * kLadderRung;
  static constexpr uint16_t kBlockAccepted = 46;  // v4: + trailing clOrdId (u64)
  static constexpr uint16_t kBlockExecuted = 54;  // v4: + trailing clOrdId (u64)
  static constexpr uint16_t kBlockTrade = 53;
  static constexpr uint16_t kBlockCanceled = 45;  // v4: + clOrdId (u64); v9: + leavesQty, cumQty (i64 each)
  static constexpr uint16_t kBlockRejected = 37;  // v4: + clOrdId (u64); v9: + cumQty (i64)
  static constexpr uint16_t kBlockReplaced = 45;  // v4: + trailing clOrdId (u64)
  static constexpr uint16_t kBlockTriggered = 28;
  static constexpr uint16_t kBlockFillHeld = 69;      // v7: + trailing clOrdId (u64)
  static constexpr uint16_t kBlockFillRejected = 60;  // v7: + trailing clOrdId (u64)
  static constexpr uint16_t kBlockSnapshotRequired = 8;
  static constexpr uint16_t kBlockSnapshotEndV1 = 28;  // pre-lastSeq layout (schema v1)
  static constexpr uint16_t kBlockSnapshotEnd = 36;    // v2: + trailing lastSeq (u64)
  static constexpr uint16_t kBlockBalanceUpdate = 35;
  // orderId(8) + symbol(4) + reason(1) + wasReplace(1) + seq(8)
  static constexpr uint16_t kBlockCancelRejected = 22;
  // account(8) + symbol(4) + qtyDelta(8) + qtyAfter(8) + entryAfter(8)
  // + reason(1) + seq(8) + note(32)
  static constexpr uint16_t kBlockPositionAdjusted = 77;

  // The ladder is now the longest frame this codec produces, by a wide
  // margin: it carries a whole block of rungs where every other inbound
  // message carries one order.
  static constexpr size_t kMaxSize =
      sbe::kHeaderSize + (kBlockEnter > kBlockQuoteLadder ? kBlockEnter : kBlockQuoteLadder);

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
      sbe::putI64(out, n->expiryNs.raw());
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
    else if (const auto* l = std::get_if<venue::QuoteLadder>(&cmd))
    {
      sbe::putHeader(out, kBlockQuoteLadder, u16(InTmpl::QuoteLadder), kSchemaId, kVersion);
      sbe::putU64(out, l->accountId);
      sbe::putU64(out, l->clientOrderId);
      sbe::putI64(out, l->expiryNs.raw());
      sbe::putI64(out, l->visibleQuantity.raw());
      sbe::putU64(out, l->bidIdBase);
      sbe::putU64(out, l->askIdBase);
      sbe::putU32(out, l->symbol);
      // The clamped count, not the raw byte: the wire carries as many rungs
      // as the block has, so a count past the end would name rungs that are
      // not there.
      sbe::putU8(out, quoteLadderLiveLevels(*l));
      sbe::putU8(out, static_cast<uint8_t>(l->stp));
      sbe::putU8(out, l->lastLook ? 1 : 0);
      sbe::putU8(out, l->postOnly ? 1 : 0);
      sbe::putU8(out, l->reduceOnly ? 1 : 0);
      sbe::putU8(out, static_cast<uint8_t>(l->tif));
      for (uint8_t i = 0; i < kQuoteLadderLevels; ++i)
      {
        sbe::putI64(out, l->level[i].bidPrice.raw());
        sbe::putI64(out, l->level[i].bidQty.raw());
        sbe::putI64(out, l->level[i].askPrice.raw());
        sbe::putI64(out, l->level[i].askQty.raw());
      }
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
        o.expiryNs = SeqNanos::fromRaw(sbe::getI64(b + 76));
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
      case InTmpl::QuoteLadder:
      {
        if (h.blockLength < kBlockQuoteLadder)
        {
          return std::nullopt;
        }
        venue::QuoteLadder l;
        l.accountId = sbe::getU64(b + 0);
        l.clientOrderId = sbe::getU64(b + 8);
        l.expiryNs = SeqNanos::fromRaw(sbe::getI64(b + 16));
        l.visibleQuantity = Quantity::fromRaw(sbe::getI64(b + 24));
        l.bidIdBase = sbe::getU64(b + 32);
        l.askIdBase = sbe::getU64(b + 40);
        l.symbol = static_cast<SymbolId>(sbe::getU32(b + 48));
        // A count past the block is clamped here rather than refused, the
        // same answer the engine and the journal give it: the frame carries
        // exactly kQuoteLadderLevels rungs, so there is nothing a larger
        // count could name.
        l.levels = b[52] > kQuoteLadderLevels ? kQuoteLadderLevels : b[52];
        l.stp = static_cast<STPMode>(b[53]);
        l.lastLook = b[54] != 0;
        l.postOnly = b[55] != 0;
        l.reduceOnly = b[56] != 0;
        l.tif = static_cast<TimeInForce>(b[57]);
        for (uint8_t i = 0; i < kQuoteLadderLevels; ++i)
        {
          const uint8_t* r = b + kLadderHead + i * kLadderRung;
          l.level[i].bidPrice = Price::fromRaw(sbe::getI64(r + 0));
          l.level[i].bidQty = Quantity::fromRaw(sbe::getI64(r + 8));
          l.level[i].askPrice = Price::fromRaw(sbe::getI64(r + 16));
          l.level[i].askQty = Quantity::fromRaw(sbe::getI64(r + 24));
        }
        return InboundCommand{l};
      }
      default:
        break;  // session verbs (ResendRequest / AccountSnapshotRequest) are
                // handled by the gateway delivery layer, never decoded here
    }
    return std::nullopt;  // unknown / outbound template
  }

  // ---- outbound: execution report -> wire ----
  // `seq` is the per-session exec-report sequence number (schema v1, trailing
  // field of every outbound root block). 0 = unsequenced (embedded/test use).
  static void encode(const OutboundEvent& ev, std::vector<uint8_t>& out, uint64_t seq = 0)
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
      sbe::putU64(out, seq);
      sbe::putU64(out, a->clientOrderId);
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
      sbe::putU64(out, seq);
      sbe::putU64(out, x->clientOrderId);
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
      sbe::putU64(out, seq);
    }
    else if (const auto* cr = std::get_if<CancelRejected>(&ev))
    {
      sbe::putHeader(out, kBlockCancelRejected, u16(OutTmpl::CancelRejected), kSchemaId, kVersion);
      sbe::putU64(out, cr->id);
      sbe::putU32(out, cr->symbol);
      sbe::putU8(out, static_cast<uint8_t>(cr->reason));
      sbe::putU8(out, cr->wasReplace ? 1 : 0);
      sbe::putU64(out, seq);
    }
    else if (const auto* pa = std::get_if<PositionAdjusted>(&ev))
    {
      sbe::putHeader(out, kBlockPositionAdjusted, u16(OutTmpl::PositionAdjusted), kSchemaId,
                     kVersion);
      sbe::putU64(out, pa->account);
      sbe::putU32(out, pa->symbol);
      sbe::putU64(out, static_cast<uint64_t>(pa->qtyDeltaRaw));
      sbe::putU64(out, static_cast<uint64_t>(pa->qtyAfterRaw));
      sbe::putU64(out, static_cast<uint64_t>(pa->entryAfterRaw));
      sbe::putU8(out, static_cast<uint8_t>(pa->reason));
      sbe::putU64(out, seq);
      for (size_t i = 0; i < kAdjustNoteLen; ++i)
      {
        sbe::putU8(out, static_cast<uint8_t>(pa->note[i]));
      }
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      sbe::putHeader(out, kBlockCanceled, u16(OutTmpl::Canceled), kSchemaId, kVersion);
      sbe::putU64(out, c->id);
      sbe::putU32(out, c->symbol);
      sbe::putU8(out, static_cast<uint8_t>(c->reason));
      sbe::putU64(out, seq);
      sbe::putU64(out, c->clientOrderId);
      sbe::putI64(out, c->leavesQty.raw());  // v9 (T058): residual this cancel killed
      sbe::putI64(out, c->cumQty.raw());     // v9 (T058): total filled before this cancel
    }
    else if (const auto* j = std::get_if<OrderRejected>(&ev))
    {
      sbe::putHeader(out, kBlockRejected, u16(OutTmpl::Rejected), kSchemaId, kVersion);
      sbe::putU64(out, j->id);
      sbe::putU32(out, j->symbol);
      sbe::putU8(out, static_cast<uint8_t>(j->reason));
      sbe::putU64(out, seq);
      sbe::putU64(out, j->clientOrderId);
      // v9 (T058): total filled before this reject. No leavesQty field: a
      // rejected order is never left resting, so it is always 0 (see
      // OrderRejected::cumQty).
      sbe::putI64(out, j->cumQty.raw());
    }
    else if (const auto* m = std::get_if<OrderModified>(&ev))
    {
      sbe::putHeader(out, kBlockReplaced, u16(OutTmpl::Replaced), kSchemaId, kVersion);
      sbe::putU64(out, m->id);
      sbe::putU32(out, m->symbol);
      sbe::putI64(out, m->price.raw());
      sbe::putI64(out, m->leavesQty.raw());
      sbe::putU8(out, m->priorityKept ? 1 : 0);
      sbe::putU64(out, seq);
      sbe::putU64(out, m->clientOrderId);
    }
    else if (const auto* g = std::get_if<OrderTriggered>(&ev))
    {
      sbe::putHeader(out, kBlockTriggered, u16(OutTmpl::Triggered), kSchemaId, kVersion);
      sbe::putU64(out, g->id);
      sbe::putU32(out, g->symbol);
      sbe::putI64(out, g->refPrice.raw());
      sbe::putU64(out, seq);
    }
    else if (const auto* fh = std::get_if<FillHeld>(&ev))
    {
      sbe::putHeader(out, kBlockFillHeld, u16(OutTmpl::FillHeld), kSchemaId, kVersion);
      sbe::putU64(out, fh->heldId);
      sbe::putU32(out, fh->symbol);
      sbe::putU64(out, fh->makerId);
      sbe::putU64(out, fh->takerId);
      sbe::putI64(out, fh->price.raw());
      sbe::putI64(out, fh->qty.raw());
      sbe::putI64(out, fh->makerDisplayAfter.raw());
      sbe::putU64(out, seq);
      sbe::putU8(out, static_cast<uint8_t>(fh->takerSide));
      sbe::putU64(out, fh->clientOrderId);
    }
    else if (const auto* fr = std::get_if<FillRejected>(&ev))
    {
      sbe::putHeader(out, kBlockFillRejected, u16(OutTmpl::FillRejected), kSchemaId, kVersion);
      sbe::putU64(out, fr->heldId);
      sbe::putU32(out, fr->symbol);
      sbe::putU64(out, fr->takerId);
      sbe::putU64(out, fr->makerId);
      sbe::putI64(out, fr->price.raw());
      sbe::putI64(out, fr->qty.raw());
      sbe::putU64(out, seq);
      sbe::putU64(out, fr->clientOrderId);
    }
    else if (const auto* bu = std::get_if<venue::BalanceUpdate>(&ev))
    {
      sbe::putHeader(out, kBlockBalanceUpdate, u16(OutTmpl::BalanceUpdate), kSchemaId, kVersion);
      sbe::putU64(out, bu->account);
      sbe::putU16(out, bu->asset);
      sbe::putI64(out, bu->availableRaw);
      sbe::putI64(out, bu->reservedRaw);
      sbe::putU8(out, static_cast<uint8_t>(bu->reason));
      sbe::putU64(out, seq);
    }
    // Events without an order-entry exec-report mapping (MmpTriggered,
    // FeeCharged, Liquidation) produce no frame. TradingStatusChanged and
    // DerivativesUpdated are instrument-wide, not account-scoped: they carry
    // no account to route to and belong on the public market-data feed
    // (market_data.h), not in a client's exec-report stream.
    //
    // A NEW event with no branch produces no frame either, and looks exactly
    // like one of the deliberate omissions above. That is how CancelRejected
    // was declared in the schema and absent from the codec for a release: the
    // client waited for bytes nobody was sending, and neither side said so.
    static_assert(std::variant_size_v<OutboundEvent> == 17,
                  "new OutboundEvent alternative: encode it above, or decide here -- with the "
                  "reason written down -- that order entry has no mapping for it");
  }

  // Per-session seq of an outbound exec report (schema v1 trailing field).
  // 0 for inbound templates, foreign schemas, or version-0 frames.
  static uint64_t seqOf(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return 0;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId || h.version < 1 || h.templateId < 10 || h.blockLength < 8 ||
        n < sbe::kHeaderSize + h.blockLength)
    {
      return 0;
    }
    return sbe::getU64(p + sbe::kHeaderSize + seqOffsetIn(h));
  }

  // Where `seq` sits inside the root block. It was appended at version 1, so
  // it ends the version-1 block -- and that stopped being the end of the block
  // when version 4 appended clOrdId after it on the five order-report
  // templates, again when version 6 appended takerSide after it on FillHeld,
  // and again when version 7 appended clOrdId after THAT on FillHeld (17
  // bytes trail seq there now) and directly after seq on FillRejected (16,
  // same width as the order-report templates, just gated by a later version).
  // Reading the last eight bytes was the old shortcut; it silently returned a
  // client's order id as a sequence number the moment a v4 frame arrived,
  // which is what the delivery tests caught. The frame says which version it
  // is, so that is what decides.
  static constexpr uint16_t seqOffsetIn(const sbe::Header& h)
  {
    const bool isCanceled = h.templateId == u16(OutTmpl::Canceled);
    const bool isRejected = h.templateId == u16(OutTmpl::Rejected);
    const bool hasClOrdIdAfterSeq =
        h.version >= 4 &&
        (h.templateId == u16(OutTmpl::Accepted) || h.templateId == u16(OutTmpl::Executed) ||
         isCanceled || isRejected || h.templateId == u16(OutTmpl::Replaced));
    const bool isFillHeld = h.templateId == u16(OutTmpl::FillHeld);
    const bool isFillRejected = h.templateId == u16(OutTmpl::FillRejected);
    uint16_t trailing = 8;
    if (hasClOrdIdAfterSeq || (isFillRejected && h.version >= 7))
    {
      trailing = 16;
    }
    else if (isFillHeld && h.version >= 7)
    {
      trailing = 17;
    }
    else if (isFillHeld && h.version >= 6)
    {
      trailing = 9;
    }
    // T058 (v9): Canceled gained trailing leavesQty+cumQty (16 bytes, after
    // clOrdId); Rejected gained trailing cumQty (8 bytes, after clOrdId).
    // Both push `seq` further from the end of the block -- gated by version,
    // exactly like every earlier addition here, so a v8 frame (still 16
    // trailing bytes on both) keeps decoding correctly.
    if (h.version >= 9)
    {
      if (isCanceled)
      {
        trailing += 16;
      }
      else if (isRejected)
      {
        trailing += 8;
      }
    }
    return h.blockLength >= trailing ? static_cast<uint16_t>(h.blockLength - trailing) : 0;
  }

  // ---- session-layer verbs (gateway delivery layer, never matched) ----
  static void encodeResendRequest(uint64_t fromSeq, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockResendRequest, u16(InTmpl::ResendRequest), kSchemaId, kVersion);
    sbe::putU64(out, fromSeq);
  }
  static std::optional<uint64_t> decodeResendRequest(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return std::nullopt;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId || h.templateId != u16(InTmpl::ResendRequest) ||
        h.blockLength < kBlockResendRequest || n < sbe::kHeaderSize + h.blockLength)
    {
      return std::nullopt;
    }
    return sbe::getU64(p + sbe::kHeaderSize);
  }
  // The snapshot request carries no fields: the session IS the account.
  static void encodeSnapshotRequest(std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSnapshotRequest, u16(InTmpl::AccountSnapshotRequest), kSchemaId,
                   kVersion);
  }
  static bool isSnapshotRequest(const uint8_t* p, size_t n)
  {
    return templateId(p, n) == u16(InTmpl::AccountSnapshotRequest);
  }
  // Session config (fire-and-forget): per-session cancel-on-disconnect.
  static void encodeSetSessionConfig(bool codEnabled, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSetSessionConfig, u16(InTmpl::SetSessionConfig), kSchemaId,
                   kVersion);
    sbe::putU8(out, codEnabled ? 1 : 0);
  }
  static std::optional<bool> decodeSetSessionConfig(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return std::nullopt;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId || h.templateId != u16(InTmpl::SetSessionConfig) ||
        h.blockLength < kBlockSetSessionConfig || n < sbe::kHeaderSize + h.blockLength)
    {
      return std::nullopt;
    }
    return p[sbe::kHeaderSize] != 0;
  }
  static void encodeSnapshotRequired(uint64_t lastSeq, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSnapshotRequired, u16(OutTmpl::SnapshotRequired), kSchemaId,
                   kVersion);
    sbe::putU64(out, lastSeq);
  }
  struct SnapshotEnd
  {
    uint64_t account{};
    int64_t positionQtyRaw{};
    int64_t positionEntryRaw{};
    uint32_t openOrders{};
    // Last assigned outbound seq of the account's exec-report stream at
    // snapshot time (schema v2, trailing field; 0 on a v1 frame). Snapshot
    // frames themselves are unsequenced and never in the resend log; this is
    // the exact point the client resumes gap detection from.
    uint64_t lastSeq{};
  };
  static void encodeSnapshotEnd(const SnapshotEnd& se, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSnapshotEnd, u16(OutTmpl::SnapshotEnd), kSchemaId, kVersion);
    sbe::putU64(out, se.account);
    sbe::putI64(out, se.positionQtyRaw);
    sbe::putI64(out, se.positionEntryRaw);
    sbe::putU32(out, se.openOrders);
    sbe::putU64(out, se.lastSeq);
  }
  static std::optional<SnapshotEnd> decodeSnapshotEnd(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return std::nullopt;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId || h.templateId != u16(OutTmpl::SnapshotEnd) ||
        h.blockLength < kBlockSnapshotEndV1 || n < sbe::kHeaderSize + h.blockLength)
    {
      return std::nullopt;
    }
    const uint8_t* b = p + sbe::kHeaderSize;
    SnapshotEnd se;
    se.account = sbe::getU64(b + 0);
    se.positionQtyRaw = sbe::getI64(b + 8);
    se.positionEntryRaw = sbe::getI64(b + 16);
    se.openOrders = sbe::getU32(b + 24);
    if (h.blockLength >= kBlockSnapshotEnd)
    {
      se.lastSeq = sbe::getU64(b + 28);
    }
    return se;
  }
  // Balance change on a sequenced Deposit/Withdraw (schema v2).
  static std::optional<venue::BalanceUpdate> decodeBalanceUpdate(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return std::nullopt;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId || h.templateId != u16(OutTmpl::BalanceUpdate) ||
        h.blockLength < kBlockBalanceUpdate || n < sbe::kHeaderSize + h.blockLength)
    {
      return std::nullopt;
    }
    const uint8_t* b = p + sbe::kHeaderSize;
    venue::BalanceUpdate bu;
    bu.account = sbe::getU64(b + 0);
    bu.asset = static_cast<AssetId>(sbe::getU16(b + 8));
    bu.availableRaw = sbe::getI64(b + 10);
    bu.reservedRaw = sbe::getI64(b + 18);
    bu.reason = static_cast<BalanceReason>(b[26]);
    return bu;
  }

 private:
  static constexpr uint16_t u16(InTmpl t) { return static_cast<uint16_t>(t); }
  static constexpr uint16_t u16(OutTmpl t) { return static_cast<uint16_t>(t); }
};

}  // namespace flox::venue
