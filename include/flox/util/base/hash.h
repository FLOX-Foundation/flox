/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace flox::hash
{

// Fowler–Noll–Vo FNV-1a 64-bit (non-cryptographic)
static constexpr uint64_t FNV1A64_OFFSET = 1469598103934665603ull;
static constexpr uint64_t FNV1A64_PRIME = 1099511628211ull;

static inline uint64_t fnv1a_64(std::string_view s) noexcept
{
  uint64_t x = FNV1A64_OFFSET;
  for (unsigned char c : s)
  {
    x ^= c;
    x *= FNV1A64_PRIME;
  }
  return x;
}

static inline uint64_t fnv1a_64(const void* data, size_t n) noexcept
{
  const auto* p = static_cast<const unsigned char*>(data);
  uint64_t x = FNV1A64_OFFSET;
  for (size_t i = 0; i < n; ++i)
  {
    x ^= p[i];
    x *= FNV1A64_PRIME;
  }
  return x;
}

}  // namespace flox::hash
