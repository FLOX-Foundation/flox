/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace flox::wire
{

inline void put(std::vector<uint8_t>& o, uint64_t v, int bytes)
{
  for (int i = bytes - 1; i >= 0; --i)
  {
    o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFU));
  }
}

inline uint64_t get(const uint8_t* p, int bytes)
{
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i)
  {
    v = (v << 8) | p[i];
  }
  return v;
}

struct Reader
{
  const uint8_t* p{};
  size_t n{};
  size_t o{};
  bool ok{true};

  bool need(size_t k) const { return o + k <= n; }

  uint8_t u8()
  {
    if (!need(1))
    {
      ok = false;
      return 0;
    }
    return p[o++];
  }
  uint64_t u(int bytes)
  {
    if (!need(static_cast<size_t>(bytes)))
    {
      ok = false;
      return 0;
    }
    const uint64_t v = get(p + o, bytes);
    o += static_cast<size_t>(bytes);
    return v;
  }
  int64_t i(int bytes) { return static_cast<int64_t>(u(bytes)); }
};

}  // namespace flox::wire
