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

class SbeMdCodec
{
 public:
  static constexpr uint16_t kSchemaId = 91;
  static constexpr uint16_t kVersion = 0;
  static constexpr size_t kHeaderSize = sbe::kHeaderSize;

  // Template ids mirror MdType (order-preserving bijection): templateId = MdType+1.
  enum class Tmpl : uint16_t
  {
    AddOrder = 1,
    Trade = 2,
    Executed = 3,
    Cancel = 4,
    Replace = 5,
    Triggered = 6,
  };

  // Root-block lengths per template (bytes after the header). Must match the
  // field lists in md-sbe.xml.
  static constexpr uint16_t kBlockOrder = 8 + 4 + 8 + 1 + 8 + 8;  // 37: seq,sym,id,side,px,qty
  static constexpr uint16_t kBlockTrade = kBlockOrder + 8;        // 45: + makerId
  static constexpr uint16_t kBlockTriggered = 8 + 4 + 8 + 8;      // 28: seq,sym,id,px

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
        break;
      case MdType::Trade:
        sbe::putHeader(out, kBlockTrade, tmpl(Tmpl::Trade), kSchemaId, kVersion);
        putOrderBlock(out, m);
        sbe::putU64(out, m.makerId);
        break;
      case MdType::Executed:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Executed), kSchemaId, kVersion);
        putOrderBlock(out, m);
        break;
      case MdType::Cancel:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Cancel), kSchemaId, kVersion);
        putOrderBlock(out, m);
        break;
      case MdType::Replace:
        sbe::putHeader(out, kBlockOrder, tmpl(Tmpl::Replace), kSchemaId, kVersion);
        putOrderBlock(out, m);
        break;
      case MdType::Triggered:
        sbe::putHeader(out, kBlockTriggered, tmpl(Tmpl::Triggered), kSchemaId, kVersion);
        sbe::putU64(out, m.seq);
        sbe::putU32(out, m.symbol);
        sbe::putU64(out, m.id);
        sbe::putI64(out, m.price.raw());
        break;
    }
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
        if (h.blockLength < kBlockOrder)
        {
          return false;
        }
        m = MdMessage{};
        m.type = tmplToType(static_cast<Tmpl>(h.templateId));
        getOrderBlock(b, m);
        return true;
      case Tmpl::Trade:
        if (h.blockLength < kBlockTrade)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::Trade;
        getOrderBlock(b, m);
        m.makerId = sbe::getU64(b + kBlockOrder);
        return true;
      case Tmpl::Triggered:
        if (h.blockLength < kBlockTriggered)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::Triggered;
        m.seq = sbe::getU64(b + 0);
        m.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
        m.id = sbe::getU64(b + 12);
        m.price = Price::fromRaw(sbe::getI64(b + 20));
        return true;
    }
    return false;  // unknown template
  }

 private:
  static uint16_t tmpl(Tmpl t) { return static_cast<uint16_t>(t); }
  static MdType tmplToType(Tmpl t) { return static_cast<MdType>(static_cast<uint16_t>(t) - 1); }

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
