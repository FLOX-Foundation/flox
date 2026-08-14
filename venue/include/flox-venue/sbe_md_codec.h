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

// Unicast distribution verbs (TCP, length-prefixed frames; see
// md_distribution.h). The recovery channel serves one request per connection;
// the distribution channel is a long-lived session, so it needs a way to say
// "start streaming this symbol" and "stop". ResendRequest keeps its meaning on
// both channels.
struct MdSubscribeRequest
{
  SymbolId symbol{};
  uint64_t fromSeq{};        // 0 = snapshot then increments; >0 = replay then increments
  bool snapshotOnly{false};  // serve one snapshot and drop the subscription
};

struct MdUnsubscribeRequest
{
  SymbolId symbol{};
};

// Refusal of a Subscribe/ResendRequest that cannot be served at all (today:
// an unknown symbol). A rejected request leaves no subscription behind.
enum class MdSubscribeRejectReason : uint8_t
{
  UnknownSymbol = 0,
  UnsupportedRequest = 1,
};

struct MdSubscribeReject
{
  SymbolId symbol{};
  MdSubscribeRejectReason reason{};
};

class SbeMdCodec
{
 public:
  static constexpr uint16_t kSchemaId = 91;
  static constexpr uint16_t kVersion = 3;
  static constexpr size_t kHeaderSize = sbe::kHeaderSize;

  // Template ids 1..6 mirror the ORDER-LEVEL MdTypes (templateId = MdType+1).
  // 7..10 are the recovery-channel messages, 11..13 the unicast distribution
  // verbs added in schema version 2, 14..15 the instrument-wide messages added
  // in version 3 -- those two do NOT continue the arithmetic (7 is taken), so
  // the mapping past Triggered is explicit. Versions 2 and 3 add templates and
  // TRAILING fields only, so an older reader decodes every message it knew
  // exactly as before.
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
    Subscribe = 11,
    Unsubscribe = 12,
    SubscribeReject = 13,
    TradingStatus = 14,
    DerivativesUpdate = 15,
  };

  // Root-block lengths per template (bytes after the header). Must match the
  // field lists in md-sbe.xml. Version 1 appended a trailing epoch (u64) to
  // every incremental template and version 3 appended engineTs+sendTs (2x i64)
  // after it; an older reader skips them via blockLength, and this decoder
  // accepts v0/v1/v2 frames (the absent trailing fields read as 0).
  static constexpr uint16_t kBlockOrderV0 = 8 + 4 + 8 + 1 + 8 + 8;      // 37: seq,sym,id,side,px,qty
  static constexpr uint16_t kBlockOrderV1 = kBlockOrderV0 + 8;          // 45: + epoch
  static constexpr uint16_t kBlockOrder = kBlockOrderV1 + 16;           // 61: + engineTs,sendTs
  static constexpr uint16_t kBlockTradeV0 = kBlockOrderV0 + 8;          // 45: + makerId
  static constexpr uint16_t kBlockTradeV1 = kBlockTradeV0 + 8;          // 53: + epoch
  static constexpr uint16_t kBlockTrade = kBlockTradeV1 + 16;           // 69: + engineTs,sendTs
  static constexpr uint16_t kBlockTriggeredV0 = 8 + 4 + 8 + 8;          // 28: seq,sym,id,px
  static constexpr uint16_t kBlockTriggeredV1 = kBlockTriggeredV0 + 8;  // 36: + epoch
  static constexpr uint16_t kBlockTriggered = kBlockTriggeredV1 + 16;   // 52: + engineTs,sendTs
  // seq,sym,epoch,engineTs,sendTs,status,reason,untilNs
  static constexpr uint16_t kBlockTradingStatus = 8 + 4 + 8 + 8 + 8 + 1 + 1 + 8;  // 46
  // seq,sym,epoch,engineTs,sendTs,mark,fundingRate,nextFundingNs,openInterest
  static constexpr uint16_t kBlockDerivatives = 8 + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 8;  // 68
  static constexpr uint16_t kBlockResendRequest = 4 + 8;                            // 12: sym,fromSeq
  static constexpr uint16_t kBlockSnapshotBegin = 4 + 8 + 8 + 4;                    // 24: sym,epoch,lastSeq,count
  static constexpr uint16_t kBlockSnapshotEnd = 4 + 8 + 8;                          // 20: sym,epoch,lastSeq
  static constexpr uint16_t kBlockSnapshotRequired = kBlockSnapshotEnd;
  static constexpr uint16_t kBlockSubscribe = 4 + 8 + 1;    // 13: sym,fromSeq,flags
  static constexpr uint16_t kBlockUnsubscribe = 4;          // 4: sym
  static constexpr uint16_t kBlockSubscribeReject = 4 + 1;  // 5: sym,reason

  static constexpr uint8_t kSubscribeSnapshotOnly = 0x01;  // Subscribe flags bit 0

  // Largest framed message: the derivatives block is the widest fixed block
  // after the trade block gained its timestamps.
  static constexpr size_t kMaxSize =
      kHeaderSize + (kBlockTrade > kBlockDerivatives ? kBlockTrade : kBlockDerivatives);

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
        putTimes(out, m);
        break;
      case MdType::Trade:
        sbe::putHeader(out, kBlockTrade, tmpl(Tmpl::Trade), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.makerId);
        sbe::putU64(out, m.epoch);
        putTimes(out, m);
        break;
      case MdType::Executed:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Executed), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.epoch);
        putTimes(out, m);
        break;
      case MdType::Cancel:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Cancel), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.epoch);
        putTimes(out, m);
        break;
      case MdType::Replace:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Replace), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.epoch);
        putTimes(out, m);
        break;
      case MdType::Triggered:
        sbe::putHeader(out, kBlockTriggered, tmpl(Tmpl::Triggered), kSchemaId, kVersion);
        sbe::putU64(out, m.seq);
        sbe::putU32(out, m.symbol);
        sbe::putU64(out, m.id);
        sbe::putI64(out, m.price.raw());
        sbe::putU64(out, m.epoch);
        putTimes(out, m);
        break;
      case MdType::TradingStatus:
        sbe::putHeader(out, kBlockTradingStatus, tmpl(Tmpl::TradingStatus), kSchemaId, kVersion);
        sbe::putU64(out, m.seq);
        sbe::putU32(out, m.symbol);
        sbe::putU64(out, m.epoch);
        putTimes(out, m);
        sbe::putU8(out, static_cast<uint8_t>(m.status));
        sbe::putU8(out, static_cast<uint8_t>(m.reason));
        sbe::putI64(out, m.untilNs);
        break;
      case MdType::DerivativesUpdate:
        sbe::putHeader(out, kBlockDerivatives, tmpl(Tmpl::DerivativesUpdate), kSchemaId, kVersion);
        sbe::putU64(out, m.seq);
        sbe::putU32(out, m.symbol);
        sbe::putU64(out, m.epoch);
        putTimes(out, m);
        sbe::putI64(out, m.price.raw());  // mark
        sbe::putI64(out, m.fundingRateRaw);
        sbe::putI64(out, m.nextFundingNs);
        sbe::putI64(out, m.qty.raw());  // open interest
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

  static void encode(const MdSubscribeRequest& s, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSubscribe, tmpl(Tmpl::Subscribe), kSchemaId, kVersion);
    sbe::putU32(out, s.symbol);
    sbe::putU64(out, s.fromSeq);
    sbe::putU8(out, s.snapshotOnly ? kSubscribeSnapshotOnly : 0);
  }

  static void encode(const MdUnsubscribeRequest& u, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockUnsubscribe, tmpl(Tmpl::Unsubscribe), kSchemaId, kVersion);
    sbe::putU32(out, u.symbol);
  }

  static void encode(const MdSubscribeReject& r, std::vector<uint8_t>& out)
  {
    out.clear();
    sbe::putHeader(out, kBlockSubscribeReject, tmpl(Tmpl::SubscribeReject), kSchemaId, kVersion);
    sbe::putU32(out, r.symbol);
    sbe::putU8(out, static_cast<uint8_t>(r.reason));
  }

  static bool decode(const uint8_t* p, size_t n, MdSubscribeRequest& s)
  {
    const uint8_t* b = checkFrame(p, n, Tmpl::Subscribe, kBlockSubscribe);
    if (b == nullptr)
    {
      return false;
    }
    s.symbol = static_cast<SymbolId>(sbe::getU32(b + 0));
    s.fromSeq = sbe::getU64(b + 4);
    s.snapshotOnly = (b[12] & kSubscribeSnapshotOnly) != 0;
    return true;
  }

  static bool decode(const uint8_t* p, size_t n, MdUnsubscribeRequest& u)
  {
    const uint8_t* b = checkFrame(p, n, Tmpl::Unsubscribe, kBlockUnsubscribe);
    if (b == nullptr)
    {
      return false;
    }
    u.symbol = static_cast<SymbolId>(sbe::getU32(b + 0));
    return true;
  }

  static bool decode(const uint8_t* p, size_t n, MdSubscribeReject& r)
  {
    const uint8_t* b = checkFrame(p, n, Tmpl::SubscribeReject, kBlockSubscribeReject);
    if (b == nullptr)
    {
      return false;
    }
    r.symbol = static_cast<SymbolId>(sbe::getU32(b + 0));
    r.reason = static_cast<MdSubscribeRejectReason>(b[4]);
    return true;
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
        if (h.blockLength >= kBlockOrderV1)
        {
          m.epoch = sbe::getU64(b + kBlockOrderV0);  // v1 trailing field; v0 frame -> 0
        }
        getTimes(b, h.blockLength, kBlockOrderV1, m);  // v3 trailing fields
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
        if (h.blockLength >= kBlockTradeV1)
        {
          m.epoch = sbe::getU64(b + kBlockTradeV0);
        }
        getTimes(b, h.blockLength, kBlockTradeV1, m);
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
        if (h.blockLength >= kBlockTriggeredV1)
        {
          m.epoch = sbe::getU64(b + kBlockTriggeredV0);
        }
        getTimes(b, h.blockLength, kBlockTriggeredV1, m);
        return true;
      case Tmpl::TradingStatus:
        if (h.blockLength < kBlockTradingStatus)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::TradingStatus;
        m.seq = sbe::getU64(b + 0);
        m.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
        m.epoch = sbe::getU64(b + 12);
        m.engineTsNs = sbe::getI64(b + 20);
        m.sendTsNs = sbe::getI64(b + 28);
        m.status = static_cast<flox::venue::TradingStatus>(b[36]);
        m.reason = static_cast<TradingStatusReason>(b[37]);
        m.untilNs = sbe::getI64(b + 38);
        return true;
      case Tmpl::DerivativesUpdate:
        if (h.blockLength < kBlockDerivatives)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::DerivativesUpdate;
        m.seq = sbe::getU64(b + 0);
        m.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
        m.epoch = sbe::getU64(b + 12);
        m.engineTsNs = sbe::getI64(b + 20);
        m.sendTsNs = sbe::getI64(b + 28);
        m.price = Price::fromRaw(sbe::getI64(b + 36));
        m.fundingRateRaw = sbe::getI64(b + 44);
        m.nextFundingNs = sbe::getI64(b + 52);
        m.qty = Quantity::fromRaw(sbe::getI64(b + 60));
        return true;
      case Tmpl::ResendRequest:
      case Tmpl::SnapshotBegin:
      case Tmpl::SnapshotEnd:
      case Tmpl::SnapshotRequired:
      case Tmpl::Subscribe:
      case Tmpl::Unsubscribe:
      case Tmpl::SubscribeReject:
        return false;  // session/recovery messages, not MdMessage frames
    }
    return false;  // unknown template
  }

 private:
  static uint16_t tmpl(Tmpl t) { return static_cast<uint16_t>(t); }
  // Only the order-level templates (1..6) carry the arithmetic mapping; the
  // instrument-wide ones are decoded by their own cases.
  static MdType tmplToType(Tmpl t) { return static_cast<MdType>(static_cast<uint16_t>(t) - 1); }

  // Version-3 trailing timestamps, appended after the version-1 epoch on every
  // incremental template.
  static void putTimes(std::vector<uint8_t>& o, const MdMessage& m)
  {
    sbe::putI64(o, m.engineTsNs);
    sbe::putI64(o, m.sendTsNs);
  }

  // Read them back when the sender's block is long enough to hold them; a
  // pre-version-3 frame leaves both at 0 (the same skip-by-blockLength path the
  // epoch field uses).
  static void getTimes(const uint8_t* b, uint16_t blockLength, uint16_t at, MdMessage& m)
  {
    if (blockLength >= static_cast<uint16_t>(at + 16))
    {
      m.engineTsNs = sbe::getI64(b + at);
      m.sendTsNs = sbe::getI64(b + at + 8);
    }
  }

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
