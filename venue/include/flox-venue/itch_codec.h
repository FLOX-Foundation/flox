/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/market_data.h"

#include <cstdint>
#include <vector>

namespace flox::venue
{

class ItchCodec
{
 public:
  static constexpr size_t kSize = 1 + 8 + 4 + 8 + 1 + 8 + 8 + 8;  // 46 bytes

  static void encode(const MdMessage& m, std::vector<uint8_t>& out)
  {
    out.clear();
    out.reserve(kSize);
    put(out, static_cast<uint8_t>(m.type), 1);
    put(out, m.seq, 8);
    put(out, m.symbol, 4);
    put(out, m.id, 8);
    put(out, static_cast<uint8_t>(m.side), 1);
    put(out, static_cast<uint64_t>(m.price.raw()), 8);
    put(out, static_cast<uint64_t>(m.qty.raw()), 8);
    put(out, m.makerId, 8);
  }

  static bool decode(const uint8_t* p, size_t n, MdMessage& m)
  {
    if (n < kSize)
    {
      return false;
    }
    size_t o = 0;
    m.type = static_cast<MdType>(p[o]);
    o += 1;
    m.seq = get(p + o, 8);
    o += 8;
    m.symbol = static_cast<SymbolId>(get(p + o, 4));
    o += 4;
    m.id = get(p + o, 8);
    o += 8;
    m.side = static_cast<Side>(p[o]);
    o += 1;
    m.price = Price::fromRaw(static_cast<int64_t>(get(p + o, 8)));
    o += 8;
    m.qty = Quantity::fromRaw(static_cast<int64_t>(get(p + o, 8)));
    o += 8;
    m.makerId = get(p + o, 8);
    return true;
  }

 private:
  static void put(std::vector<uint8_t>& o, uint64_t v, int bytes)
  {
    for (int i = bytes - 1; i >= 0; --i)
    {
      o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
  }
  static uint64_t get(const uint8_t* p, int bytes)
  {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
    {
      v = (v << 8) | p[i];
    }
    return v;
  }
};

}  // namespace flox::venue
