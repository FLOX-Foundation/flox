/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * SBE (Simple Binary Encoding) codec for the venue market-data feed.
 *
 * Source of truth: venue/schema/md-sbe.xml. Hand-written against that schema
 * (no Java codegen in a C++/Python build). Every message is a canonical SBE
 * message header (see flox-venue/sbe.h) followed by a fixed root block, all
 * little-endian. One template per MdType, each carrying only the fields that
 * type needs. Forward-compatible decode: the header's blockLength bounds the
 * root block, so trailing fields a newer version appended are skipped; a
 * foreign schemaId is rejected.
 *
 * This is not Nasdaq ITCH; it is the venue's own SBE schema. It replaced a
 * fixed-width homegrown record that was misleadingly named "ITCH".
 */
#pragma once

#include "flox-venue/market_data.h"
#include "flox-venue/sbe.h"

#include <cstdint>
#include <vector>

namespace flox::venue
{

// Recovery-channel messages (TCP, length-prefixed frames; see md_recovery.h).
struct MdResendRequest
{
  SymbolId symbol{};
  uint64_t fromSeq{};  // 0 = late joiner: always answered with a snapshot
};

struct MdSnapshotBegin
{
  SymbolId symbol{};
  uint64_t epoch{};
  uint64_t lastSeq{};
  uint32_t orderCount{};
};

struct MdSnapshotEnd
{
  SymbolId symbol{};
  uint64_t epoch{};
  uint64_t lastSeq{};
};

struct MdSnapshotRequired
{
  SymbolId symbol{};
  uint64_t epoch{};
  uint64_t lastSeq{};
};

class SbeMdCodec
{
 public:
  static constexpr uint16_t kSchemaId = 91;
  static constexpr uint16_t kVersion = 1;
  static constexpr size_t kHeaderSize = sbe::kHeaderSize;

  // Template ids 1..6 mirror MdType (order-preserving bijection):
  // templateId = MdType+1. 7..10 are the recovery-channel messages.
  enum class Tmpl : uint16_t
  {
    AddOrder = 1,
    Trade = 2,
    Executed = 3,
    Cancel = 4,
    Replace = 5,
    Triggered = 6,
    ResendRequest = 7,
    SnapshotBegin = 8,
    SnapshotEnd = 9,
    SnapshotRequired = 10,
  };

  // Root-block lengths per template (bytes after the header). Must match the
  // field lists in md-sbe.xml. Version 1 appended a trailing epoch (u64) to
  // every incremental template; a v0 reader skips it via blockLength, and this
  // decoder accepts v0 frames (epoch reads as 0).
  static constexpr uint16_t kBlockOrderV0 = 8 + 4 + 8 + 1 + 8 + 8;    // 37: seq,sym,id,side,px,qty
  static constexpr uint16_t kBlockOrder = kBlockOrderV0 + 8;          // 45: + epoch
  static constexpr uint16_t kBlockTradeV0 = kBlockOrderV0 + 8;        // 45: + makerId
  static constexpr uint16_t kBlockTrade = kBlockTradeV0 + 8;          // 53: + epoch
  static constexpr uint16_t kBlockTriggeredV0 = 8 + 4 + 8 + 8;        // 28: seq,sym,id,px
  static constexpr uint16_t kBlockTriggered = kBlockTriggeredV0 + 8;  // 36: + epoch
  static constexpr uint16_t kBlockResendRequest = 4 + 8;              // 12: sym,fromSeq
  static constexpr uint16_t kBlockSnapshotBegin = 4 + 8 + 8 + 4;      // 24: sym,epoch,lastSeq,count
  static constexpr uint16_t kBlockSnapshotEnd = 4 + 8 + 8;            // 20: sym,epoch,lastSeq
  static constexpr uint16_t kBlockSnapshotRequired = kBlockSnapshotEnd;

  static constexpr size_t kMaxSize = kHeaderSize + kBlockTrade;  // largest framed message

  static void encode(const MdMessage& m, std::vector<uint8_t>& out)
  {
    out.clear();
    out.reserve(kMaxSize);
    switch (m.type)
    {
      case MdType::AddOrder:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::AddOrder), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.epoch);
        break;
      case MdType::Trade:
        sbe::putHeader(out, kBlockTrade, tmpl(Tmpl::Trade), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.makerId);
        sbe::putU64(out, m.epoch);
        break;
      case MdType::Executed:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Executed), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.epoch);
        break;
      case MdType::Cancel:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Cancel), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.epoch);
        break;
      case MdType::Replace:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Replace), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.epoch);
        break;
      case MdType::Triggered:
        sbe::putHeader(out, kBlockTriggered, tmpl(Tmpl::Triggered), kSchemaId, kVersion);
        sbe::putU64(out, m.seq);
        sbe::putU32(out, m.symbol);
        sbe::putU64(out, m.id);
        sbe::putI64(out, m.price.raw());
        sbe::putU64(out, m.epoch);
        break;
    }
  }

  // ---- recovery-channel messages ----

  static void encode(const MdResendRequest& r, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockResendRequest, tmpl(Tmpl::ResendRequest), kSchemaId, kVersion);
    sbe::putU32(out, r.symbol);
    sbe::putU64(out, r.fromSeq);
  }

  static void encode(const MdSnapshotBegin& s, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSnapshotBegin, tmpl(Tmpl::SnapshotBegin), kSchemaId, kVersion);
    sbe::putU32(out, s.symbol);
    sbe::putU64(out, s.epoch);
    sbe::putU64(out, s.lastSeq);
    sbe::putU32(out, s.orderCount);
  }

  static void encode(const MdSnapshotEnd& s, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSnapshotEnd, tmpl(Tmpl::SnapshotEnd), kSchemaId, kVersion);
    sbe::putU32(out, s.symbol);
    sbe::putU64(out, s.epoch);
    sbe::putU64(out, s.lastSeq);
  }

  static void encode(const MdSnapshotRequired& s, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSnapshotRequired, tmpl(Tmpl::SnapshotRequired), kSchemaId, kVersion);
    sbe::putU32(out, s.symbol);
    sbe::putU64(out, s.epoch);
    sbe::putU64(out, s.lastSeq);
  }

  static bool decode(const uint8_t* p, size_t n, MdResendRequest& r)
  {
    const uint8_t* b = checkFrame(p, n, Tmpl::ResendRequest, kBlockResendRequest);
    if (b == nullptr)
    {
      return false;
    }
    r.symbol = static_cast<SymbolId>(sbe::getU32(b + 0));
    r.fromSeq = sbe::getU64(b + 4);
    return true;
  }

  static bool decode(const uint8_t* p, size_t n, MdSnapshotBegin& s)
  {
    const uint8_t* b = checkFrame(p, n, Tmpl::SnapshotBegin, kBlockSnapshotBegin);
    if (b == nullptr)
    {
      return false;
    }
    s.symbol = static_cast<SymbolId>(sbe::getU32(b + 0));
    s.epoch = sbe::getU64(b + 4);
    s.lastSeq = sbe::getU64(b + 12);
    s.orderCount = sbe::getU32(b + 20);
    return true;
  }

  static bool decode(const uint8_t* p, size_t n, MdSnapshotEnd& s)
  {
    const uint8_t* b = checkFrame(p, n, Tmpl::SnapshotEnd, kBlockSnapshotEnd);
    if (b == nullptr)
    {
      return false;
    }
    s.symbol = static_cast<SymbolId>(sbe::getU32(b + 0));
    s.epoch = sbe::getU64(b + 4);
    s.lastSeq = sbe::getU64(b + 12);
    return true;
  }

  static bool decode(const uint8_t* p, size_t n, MdSnapshotRequired& s)
  {
    const uint8_t* b = checkFrame(p, n, Tmpl::SnapshotRequired, kBlockSnapshotRequired);
    if (b == nullptr)
    {
      return false;
    }
    s.symbol = static_cast<SymbolId>(sbe::getU32(b + 0));
    s.epoch = sbe::getU64(b + 4);
    s.lastSeq = sbe::getU64(b + 12);
    return true;
  }

  // Template id of a framed message; 0 on a short buffer or foreign schema.
  static uint16_t templateId(const uint8_t* p, size_t n)
  {
    if (n < sbe::kHeaderSize)
    {
      return 0;
    }
    const sbe::Header h = sbe::readHeader(p);
    return h.schemaId == kSchemaId ? h.templateId : 0;
  }

  // Decode one framed message. Returns false on a short buffer, a foreign
  // schema, an unknown template, or an acting block too short for the fields.
  static bool decode(const uint8_t* p, size_t n, MdMessage& m)
  {
    if (n < sbe::kHeaderSize)
    {
      return false;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId)
    {
      return false;
    }
    if (n < sbe::kHeaderSize + h.blockLength)
    {
      return false;  // truncated: the advertised block is not fully present
    }
    const uint8_t* b = p + sbe::kHeaderSize;

    switch (static_cast<Tmpl>(h.templateId))
    {
      case Tmpl::AddOrder:
      case Tmpl::Executed:
      case Tmpl::Cancel:
      case Tmpl::Replace:
        if (h.blockLength < kBlockOrderV0)
        {
          return false;
        }
        m = MdMessage{};
        m.type = tmplToType(static_cast<Tmpl>(h.templateId));
        getOrderBlock(b, m);
        if (h.blockLength >= kBlockOrder)
        {
          m.epoch = sbe::getU64(b + kBlockOrderV0);  // v1 trailing field; v0 frame -> 0
        }
        return true;
      case Tmpl::Trade:
        if (h.blockLength < kBlockTradeV0)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::Trade;
        getOrderBlock(b, m);
        m.makerId = sbe::getU64(b + kBlockOrderV0);
        if (h.blockLength >= kBlockTrade)
        {
          m.epoch = sbe::getU64(b + kBlockTradeV0);
        }
        return true;
      case Tmpl::Triggered:
        if (h.blockLength < kBlockTriggeredV0)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::Triggered;
        m.seq = sbe::getU64(b + 0);
        m.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
        m.id = sbe::getU64(b + 12);
        m.price = Price::fromRaw(sbe::getI64(b + 20));
        if (h.blockLength >= kBlockTriggered)
        {
          m.epoch = sbe::getU64(b + kBlockTriggeredV0);
        }
        return true;
      case Tmpl::ResendRequest:
      case Tmpl::SnapshotBegin:
      case Tmpl::SnapshotEnd:
      case Tmpl::SnapshotRequired:
        return false;  // recovery-channel messages, not MdMessage frames
    }
    return false;  // unknown template
  }

 private:
  static uint16_t tmpl(Tmpl t) { return static_cast<uint16_t>(t); }
  static MdType tmplToType(Tmpl t) { return static_cast<MdType>(static_cast<uint16_t>(t) - 1); }

  // Validate header + template + block bounds; return the root block, or null.
  static const uint8_t* checkFrame(const uint8_t* p, size_t n, Tmpl expected, uint16_t minBlock)
  {
    if (n < sbe::kHeaderSize)
    {
      return nullptr;
    }
    const sbe::Header h = sbe::readHeader(p);
    if (h.schemaId != kSchemaId || h.templateId != tmpl(expected) || h.blockLength < minBlock ||
        n < sbe::kHeaderSize + h.blockLength)
    {
      return nullptr;
    }
    return p + sbe::kHeaderSize;
  }

  // Shared seq,symbol,orderId,side,price,qty block (37 bytes).
  static void putOrderBlock(std::vector<uint8_t>& o, const MdMessage& m)
  {
    sbe::putU64(o, m.seq);
    sbe::putU32(o, m.symbol);
    sbe::putU64(o, m.id);
    sbe::putU8(o, static_cast<uint8_t>(m.side));
    sbe::putI64(o, m.price.raw());
    sbe::putI64(o, m.qty.raw());
  }
  static void getOrderBlock(const uint8_t* b, MdMessage& m)
  {
    m.seq = sbe::getU64(b + 0);
    m.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
    m.id = sbe::getU64(b + 12);
    m.side = static_cast<Side>(b[20]);
    m.price = Price::fromRaw(sbe::getI64(b + 21));
    m.qty = Quantity::fromRaw(sbe::getI64(b + 29));
  }
};

}  // namespace flox::venue
