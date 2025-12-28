/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"

#include <chrono>
#include <cstdint>
#include <functional>

namespace flox
{

struct TimeframeId
{
  BarType type;
  uint64_t param;  // nanoseconds for Time, count for Tick, threshold for Volume, etc.

  constexpr TimeframeId() : type(BarType::Time), param(0) {}

  constexpr TimeframeId(BarType t, uint64_t p) : type(t), param(p) {}

  static constexpr TimeframeId time(std::chrono::seconds s)
  {
    return TimeframeId(BarType::Time, static_cast<uint64_t>(s.count()) * 1'000'000'000ULL);
  }

  static constexpr TimeframeId tick(uint64_t count)
  {
    return TimeframeId(BarType::Tick, count);
  }

  static constexpr TimeframeId volume(uint64_t threshold)
  {
    return TimeframeId(BarType::Volume, threshold);
  }

  static constexpr TimeframeId renko(uint64_t brickSizeTicks)
  {
    return TimeframeId(BarType::Renko, brickSizeTicks);
  }

  static constexpr TimeframeId range(uint64_t rangeTicks)
  {
    return TimeframeId(BarType::Range, rangeTicks);
  }

  constexpr bool operator==(const TimeframeId& other) const noexcept
  {
    return type == other.type && param == other.param;
  }

  constexpr bool operator!=(const TimeframeId& other) const noexcept
  {
    return !(*this == other);
  }

  constexpr bool operator<(const TimeframeId& other) const noexcept
  {
    if (type != other.type)
    {
      return type < other.type;
    }
    return param < other.param;
  }
};

namespace timeframe
{

using namespace std::chrono_literals;

inline constexpr TimeframeId S1 = TimeframeId::time(1s);
inline constexpr TimeframeId S5 = TimeframeId::time(5s);
inline constexpr TimeframeId S15 = TimeframeId::time(15s);
inline constexpr TimeframeId S30 = TimeframeId::time(30s);

inline constexpr TimeframeId M1 = TimeframeId::time(60s);
inline constexpr TimeframeId M3 = TimeframeId::time(180s);
inline constexpr TimeframeId M5 = TimeframeId::time(300s);
inline constexpr TimeframeId M15 = TimeframeId::time(900s);
inline constexpr TimeframeId M30 = TimeframeId::time(1800s);

inline constexpr TimeframeId H1 = TimeframeId::time(3600s);
inline constexpr TimeframeId H2 = TimeframeId::time(7200s);
inline constexpr TimeframeId H4 = TimeframeId::time(14400s);
inline constexpr TimeframeId H8 = TimeframeId::time(28800s);
inline constexpr TimeframeId H12 = TimeframeId::time(43200s);

inline constexpr TimeframeId D1 = TimeframeId::time(86400s);
inline constexpr TimeframeId W1 = TimeframeId::time(604800s);

}  // namespace timeframe

}  // namespace flox

template <>
struct std::hash<flox::TimeframeId>
{
  std::size_t operator()(const flox::TimeframeId& tf) const noexcept
  {
    std::size_t h1 = std::hash<int>{}(static_cast<int>(tf.type));
    std::size_t h2 = std::hash<uint64_t>{}(tf.param);
    return h1 ^ (h2 << 1);
  }
};
