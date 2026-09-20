/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) -- integrity checksums for
 * on-disk records and wire frames. The table is built at compile time.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace flox::util
{

namespace detail
{
// Built at compile time. The table used to be built on first use, behind a
// plain bool: every thread that had not seen the flag set built the table
// again, over the copy another thread was reading -- a data race, benign in
// practice only because each writer wrote the same bytes, and undefined
// behaviour regardless. Nothing about the table needs a runtime decision, so
// there is nothing to synchronise and no first-use branch left on the
// checksum path.
constexpr std::array<uint32_t, 256> makeCrc32Table() noexcept
{
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i)
  {
    uint32_t c = i;
    for (int j = 0; j < 8; ++j)
    {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

inline constexpr std::array<uint32_t, 256> kCrc32Table = makeCrc32Table();
}  // namespace detail

class Crc32
{
 public:
  static uint32_t compute(const void* data, size_t size) noexcept
  {
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
  static constexpr const std::array<uint32_t, 256>& _table = detail::kCrc32Table;
};

}  // namespace flox::util
