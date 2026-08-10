/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) -- integrity checksums for
 * on-disk records and wire frames. Table-initialised on first use.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace flox::util
{

class Crc32
{
 public:
  static uint32_t compute(const void* data, size_t size) noexcept
  {
    initTable();
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
      crc = _table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
  }

  static uint32_t compute(std::span<const std::byte> data) noexcept
  {
    return compute(data.data(), data.size());
  }

 private:
  static inline uint32_t _table[256]{};
  static inline bool _initialized{false};

  static void initTable() noexcept
  {
    if (_initialized)
    {
      return;
    }
    for (uint32_t i = 0; i < 256; ++i)
    {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j)
      {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      _table[i] = c;
    }
    _initialized = true;
  }
};

}  // namespace flox::util
