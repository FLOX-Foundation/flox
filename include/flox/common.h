/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/util/base/decimal.h"

#include <chrono>
#include <cstdint>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#endif

namespace flox
{

enum class InstrumentType
{
  Spot,
  Future,
  Inverse,
  Option
};

enum class OptionType
{
  CALL,
  PUT
};

enum class OrderType : uint8_t
{
  LIMIT = 0,
  MARKET = 1,
  STOP_MARKET = 2,
  STOP_LIMIT = 3,
  TAKE_PROFIT_MARKET = 4,
  TAKE_PROFIT_LIMIT = 5,
  TRAILING_STOP = 6,
  ICEBERG = 7,
};

enum class TimeInForce : uint8_t
{
  GTC = 0,        // Good Till Cancel (default)
  IOC = 1,        // Immediate Or Cancel
  FOK = 2,        // Fill Or Kill
  GTD = 3,        // Good Till Date
  POST_ONLY = 4,  // Maker only
};

enum class Side
{
  BUY,
  SELL
};

using SymbolId = uint32_t;
using OrderId = uint64_t;
using ExchangeId = uint16_t;

static constexpr ExchangeId InvalidExchangeId = 0xFFFF;

enum class VenueType : uint8_t
{
  CentralizedExchange,
  AmmDex,
  HybridDex
};

struct PriceTag
{
};
struct QuantityTag
{
};
struct VolumeTag
{
};

// tick = 0.00000001 (8 decimals)
using Price = Decimal<PriceTag, 100'000'000, 1>;
using Quantity = Decimal<QuantityTag, 100'000'000, 1>;
using Volume = Decimal<VolumeTag, 100'000'000, 1>;

inline Volume operator*(Quantity qty, Price px)
{
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
  // GCC/Clang on Linux/Mac - use native 128-bit arithmetic
  using i128 = __int128_t;
  return Volume::fromRaw(
      static_cast<int64_t>((i128)qty.raw() * (i128)px.raw() / (i128)Volume::Scale));
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
  // MSVC x64: use _umul128 + _udiv128 intrinsics
  uint64_t hi;
  uint64_t lo = _umul128(static_cast<uint64_t>(qty.raw()), static_cast<uint64_t>(px.raw()), &hi);
  uint64_t rem;
  uint64_t result = _udiv128(hi, lo, Volume::Scale, &rem);
  return Volume::fromRaw(static_cast<int64_t>(result));
#else
  // Portable fallback: split multiplication to avoid overflow
  return Volume::fromRaw((qty.raw() / Volume::Scale) * px.raw() +
                         (qty.raw() % Volume::Scale) * px.raw() / Volume::Scale);
#endif
}

inline Volume operator*(Price px, Quantity qty)
{
  return qty * px;
}

inline Price operator/(Volume vol, Quantity qty)
{
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
  // GCC/Clang on Linux/Mac - use native 128-bit arithmetic
  using i128 = __int128_t;
  return Price::fromRaw(
      static_cast<int64_t>((i128)vol.raw() * (i128)Price::Scale / (i128)qty.raw()));
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
  // MSVC x64: use _umul128 + _udiv128 intrinsics
  uint64_t hi;
  uint64_t lo = _umul128(static_cast<uint64_t>(vol.raw()), Price::Scale, &hi);
  uint64_t rem;
  uint64_t result = _udiv128(hi, lo, static_cast<uint64_t>(qty.raw()), &rem);
  return Price::fromRaw(static_cast<int64_t>(result));
#else
  // Portable fallback
  return Price::fromRaw((vol.raw() / qty.raw()) * Price::Scale +
                        (vol.raw() % qty.raw()) * Price::Scale / qty.raw());
#endif
}

inline Quantity operator/(Volume vol, Price px)
{
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
  // GCC/Clang on Linux/Mac - use native 128-bit arithmetic
  using i128 = __int128_t;
  return Quantity::fromRaw(
      static_cast<int64_t>((i128)vol.raw() * (i128)Quantity::Scale / (i128)px.raw()));
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
  // MSVC x64: use _umul128 + _udiv128 intrinsics
  uint64_t hi;
  uint64_t lo = _umul128(static_cast<uint64_t>(vol.raw()), Quantity::Scale, &hi);
  uint64_t rem;
  uint64_t result = _udiv128(hi, lo, static_cast<uint64_t>(px.raw()), &rem);
  return Quantity::fromRaw(static_cast<int64_t>(result));
#else
  // Portable fallback
  return Quantity::fromRaw((vol.raw() / px.raw()) * Quantity::Scale +
                           (vol.raw() % px.raw()) * Quantity::Scale / px.raw());
#endif
}

}  // namespace flox
