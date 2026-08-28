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
#include <type_traits>

namespace flox
{

// Two clock domains, two types that refuse to mix.
//
// These were plain aliases, and an alias lets a steady_clock reading be
// assigned into a wall-clock field without a sound. The difference between the
// domains is not subtle -- it is decades -- and it surfaces as corrupted data,
// not as a crash: one tape connector filled a wall-clock field from
// steady_clock and produced a day of book records 56 years adrift of their own
// trades, unreadable in time order. The same latent mistake sat in a second
// connector, masked only by its venue supplying a timestamp.
//
// The cure follows the Price/Quantity playbook: an 8-byte trivially-copyable
// wrapper, explicit fromRaw()/raw() at the boundaries, arithmetic only within
// a domain. Cross-domain assignment and subtraction now fail to compile.
// DurationNs is the result of same-domain subtraction and is deliberately
// domain-free: an interval has no epoch.
//
// Wire formats and memcpy'd structs are unaffected: the layout is exactly the
// int64/uint64 it always was (static_asserts below).
struct DurationNs
{
  int64_t ns{0};

  constexpr DurationNs() = default;
  constexpr explicit DurationNs(int64_t v) : ns(v) {}

  constexpr int64_t count() const noexcept { return ns; }
  constexpr auto operator<=>(const DurationNs&) const = default;
  constexpr DurationNs operator+(DurationNs o) const noexcept { return DurationNs{ns + o.ns}; }
  constexpr DurationNs operator-(DurationNs o) const noexcept { return DurationNs{ns - o.ns}; }
};

namespace detail
{

// One implementation, two incompatible instantiations: Tag makes
// Stamp<UnixTag> and Stamp<MonoTag> unrelated types, which is the entire
// point.
template <class Tag, class Rep>
struct Stamp
{
  Rep _raw{0};

  constexpr Stamp() = default;

  // Explicit both ways. An implicit path in either direction reopens the
  // cross-domain route through the underlying integer, and subtraction of
  // mixed domains would compile again.
  constexpr explicit Stamp(Rep v) : _raw(v) {}
  static constexpr Stamp fromRaw(Rep v) noexcept { return Stamp{v}; }
  constexpr Rep raw() const noexcept { return _raw; }

  constexpr explicit operator bool() const noexcept { return _raw != 0; }
  constexpr auto operator<=>(const Stamp&) const = default;

  // Same-domain interval; the only subtraction that exists.
  constexpr DurationNs operator-(Stamp o) const noexcept
  {
    return DurationNs{static_cast<int64_t>(_raw) - static_cast<int64_t>(o._raw)};
  }
  constexpr Stamp operator+(DurationNs d) const noexcept
  {
    return Stamp{static_cast<Rep>(static_cast<int64_t>(_raw) + d.ns)};
  }
  constexpr Stamp operator-(DurationNs d) const noexcept
  {
    return Stamp{static_cast<Rep>(static_cast<int64_t>(_raw) - d.ns)};
  }
};

struct UnixTag
{
};
struct MonoTag
{
};

}  // namespace detail

using UnixNanos = detail::Stamp<detail::UnixTag, int64_t>;
using MonoNanos = detail::Stamp<detail::MonoTag, uint64_t>;

static_assert(sizeof(UnixNanos) == 8 && std::is_trivially_copyable_v<UnixNanos>);
static_assert(sizeof(MonoNanos) == 8 && std::is_trivially_copyable_v<MonoNanos>);

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

// Venue-supplied millisecond epochs land in typed fields; one name for the
// conversion keeps fromRaw noise out of every connector.
inline UnixNanos msToUnixNs(int64_t ms) noexcept { return UnixNanos::fromRaw(ms * kNsPerMs); }

// Wall clock, typed. This exists because the untyped era let connectors reach
// for nowNsMonotonic() when a venue supplied no timestamp -- a steady-clock
// reading in a wall-clock field, invisible until someone diffed it against a
// real epoch. If there is no venue timestamp, the honest fallback is this.
inline UnixNanos nowUnixNanos() noexcept
{
  return UnixNanos::fromRaw(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
}

// Monotonic, typed.
inline MonoNanos nowMonoNanos() noexcept { return MonoNanos::fromRaw(nowNsMonotonic()); }
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

inline TimePoint fromUnixNs(UnixNanos ts) noexcept { return fromUnixNs(ts.raw()); }

}  // namespace flox
