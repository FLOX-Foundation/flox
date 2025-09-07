/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace flox
{

using UnixNanos = int64_t;
using MonoNanos = uint64_t;

using FloxClock = std::chrono::steady_clock;
using TimePoint = FloxClock::time_point;

inline TimePoint now() noexcept { return FloxClock::now(); }

constexpr int64_t kNsPerMs = 1'000'000;

inline int64_t nowNsMonotonic() noexcept
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(now().time_since_epoch()).count();
}

inline int64_t msToNs(int64_t ms) noexcept { return ms * kNsPerMs; }
inline int64_t nsToMsFloor(int64_t ns) noexcept { return ns / kNsPerMs; }

inline std::atomic<int64_t>& unix_to_flox_offset_ns()
{
  static std::atomic<int64_t> off{0};
  return off;
}

inline void init_timebase_mapping()
{
  using namespace std::chrono;

  const auto flox_ns = duration_cast<nanoseconds>(now().time_since_epoch()).count();
  const auto unix_ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();

  unix_to_flox_offset_ns().store(flox_ns - unix_ns, std::memory_order_relaxed);
}

inline int64_t unixMsToFloxNs(int64_t ms_epoch) noexcept
{
  return ms_epoch * kNsPerMs + unix_to_flox_offset_ns().load(std::memory_order_relaxed);
}

inline int64_t unixNsToFloxNs(int64_t ns_epoch) noexcept
{
  return ns_epoch + unix_to_flox_offset_ns().load(std::memory_order_relaxed);
}

inline TimePoint fromFloxNs(int64_t flox_ns) noexcept
{
  return TimePoint(FloxClock::duration(flox_ns));
}

inline TimePoint fromUnixMs(int64_t ms_epoch) noexcept
{
  return fromFloxNs(unixMsToFloxNs(ms_epoch));
}

inline TimePoint fromUnixNs(int64_t ns_epoch) noexcept
{
  return fromFloxNs(unixNsToFloxNs(ns_epoch));
}

}  // namespace flox
