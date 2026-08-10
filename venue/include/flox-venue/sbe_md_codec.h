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
 * Source of truth: venue/schema/md-sbe.xml. This is a hand-written codec that
 * honours the SBE wire contract rather than a generated one (no Java codegen in
 * a C++/Python build): every message is a canonical SBE message header
 * (blockLength:u16, templateId:u16, schemaId:u16, version:u16) followed by a
 * fixed root block, all little-endian. One template per MdType, each carrying
 * only the fields that type needs.
 *
 * Forward compatibility is real: a decoder trusts the header's blockLength to
 * find where the root block ends, reads the fields it knows, and skips any
 * trailing fields a newer schema version appended. A message whose acting block
 * is shorter than the fields this reader needs is treated as truncated.
 *
 * This is not Nasdaq ITCH; it is the venue's own SBE schema. It replaced a
 * fixed-width homegrown record that was misleadingly named "ITCH".
 */
#pragma once

#include "flox-venue/market_data.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace flox::venue
{

class SbeMdCodec
{
 public:
  static constexpr uint16_t kSchemaId = 91;
  static constexpr uint16_t kVersion = 0;
  static constexpr size_t kHeaderSize = 8;  // blockLength+templateId+schemaId+version

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
        header(out, Tmpl::AddOrder, kBlockOrder);
        putOrderBlock(out, m);
        break;
      case MdType::Trade:
        header(out, Tmpl::Trade, kBlockTrade);
        putOrderBlock(out, m);
        putU64(out, m.makerId);
        break;
      case MdType::Executed:
        header(out, Tmpl::Executed, kBlockOrder);
        putOrderBlock(out, m);
        break;
      case MdType::Cancel:
        header(out, Tmpl::Cancel, kBlockOrder);
        putOrderBlock(out, m);
        break;
      case MdType::Replace:
        header(out, Tmpl::Replace, kBlockOrder);
        putOrderBlock(out, m);
        break;
      case MdType::Triggered:
        header(out, Tmpl::Triggered, kBlockTriggered);
        putU64(out, m.seq);
        putU32(out, m.symbol);
        putU64(out, m.id);
        putI64(out, m.price.raw());
        break;
    }
  }

  // Decode one framed message. Returns false on a short buffer, an unknown
  // template, or an acting block too short for the template's fields.
  static bool decode(const uint8_t* p, size_t n, MdMessage& m)
  {
    if (n < kHeaderSize)
    {
      return false;
    }
    const uint16_t blockLength = getU16(p + 0);
    const uint16_t templateId = getU16(p + 2);
    // schemaId at +4, version at +6: read for validation; unknown schema is not ours.
    if (getU16(p + 4) != kSchemaId)
    {
      return false;
    }
    if (n < kHeaderSize + blockLength)
    {
      return false;  // truncated: the advertised block is not fully present
    }
    const uint8_t* b = p + kHeaderSize;

    switch (static_cast<Tmpl>(templateId))
    {
      case Tmpl::AddOrder:
      case Tmpl::Executed:
      case Tmpl::Cancel:
      case Tmpl::Replace:
        if (blockLength < kBlockOrder)
        {
          return false;
        }
        m = MdMessage{};
        m.type = tmplToType(static_cast<Tmpl>(templateId));
        getOrderBlock(b, m);
        return true;
      case Tmpl::Trade:
        if (blockLength < kBlockTrade)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::Trade;
        getOrderBlock(b, m);
        m.makerId = getU64(b + kBlockOrder);
        return true;
      case Tmpl::Triggered:
        if (blockLength < kBlockTriggered)
        {
          return false;
        }
        m = MdMessage{};
        m.type = MdType::Triggered;
        m.seq = getU64(b + 0);
        m.symbol = static_cast<SymbolId>(getU32(b + 8));
        m.id = getU64(b + 12);
        m.price = Price::fromRaw(getI64(b + 20));
        return true;
    }
    return false;  // unknown template
  }

 private:
  static MdType tmplToType(Tmpl t)
  {
    return static_cast<MdType>(static_cast<uint16_t>(t) - 1);
  }

  static void header(std::vector<uint8_t>& o, Tmpl tmpl, uint16_t blockLength)
  {
    putU16(o, blockLength);
    putU16(o, static_cast<uint16_t>(tmpl));
    putU16(o, kSchemaId);
    putU16(o, kVersion);
  }

  // Shared seq,symbol,orderId,side,price,qty block (37 bytes).
  static void putOrderBlock(std::vector<uint8_t>& o, const MdMessage& m)
  {
    putU64(o, m.seq);
    putU32(o, m.symbol);
    putU64(o, m.id);
    o.push_back(static_cast<uint8_t>(m.side));
    putI64(o, m.price.raw());
    putI64(o, m.qty.raw());
  }
  static void getOrderBlock(const uint8_t* b, MdMessage& m)
  {
    m.seq = getU64(b + 0);
    m.symbol = static_cast<SymbolId>(getU32(b + 8));
    m.id = getU64(b + 12);
    m.side = static_cast<Side>(b[20]);
    m.price = Price::fromRaw(getI64(b + 21));
    m.qty = Quantity::fromRaw(getI64(b + 29));
  }

  // Little-endian primitive I/O (SBE canonical byte order).
  static void putU16(std::vector<uint8_t>& o, uint16_t v)
  {
    o.push_back(static_cast<uint8_t>(v & 0xFF));
    o.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  }
  static void putU32(std::vector<uint8_t>& o, uint32_t v)
  {
    for (int i = 0; i < 4; ++i)
    {
      o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
  }
  static void putU64(std::vector<uint8_t>& o, uint64_t v)
  {
    for (int i = 0; i < 8; ++i)
    {
      o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
  }
  static void putI64(std::vector<uint8_t>& o, int64_t v)
  {
    putU64(o, static_cast<uint64_t>(v));
  }

  static uint16_t getU16(const uint8_t* p)
  {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
  }
  static uint32_t getU32(const uint8_t* p)
  {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
    {
      v |= static_cast<uint32_t>(p[i]) << (8 * i);
    }
    return v;
  }
  static uint64_t getU64(const uint8_t* p)
  {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
      v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
  }
  static int64_t getI64(const uint8_t* p)
  {
    return static_cast<int64_t>(getU64(p));
  }
};

}  // namespace flox::venue
