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
#include <cstddef>
#include <memory_resource>

namespace flox::memory
{

// Counts every allocation that escapes to the heap.
//
// Pools back their pmr vectors with a fixed inline buffer, but a
// monotonic_buffer_resource silently falls back to its upstream once that
// buffer is exhausted -- so a book deeper than the pool was sized for turns
// into a malloc, and with no instrumentation it turns into one during
// trading rather than at startup. Wrapping the upstream makes that visible:
// a non-zero count means the inline buffer was too small for the traffic.
//
// Process-wide aggregates are exposed for the startup memory report, same
// shape as LargeArena's reporting.
class CountingResource : public std::pmr::memory_resource
{
 public:
  explicit CountingResource(std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
      : _upstream(upstream)
  {
  }

  uint64_t allocations() const noexcept { return _allocations.load(std::memory_order_relaxed); }
  uint64_t bytes() const noexcept { return _bytes.load(std::memory_order_relaxed); }

  static uint64_t totalAllocations() noexcept
  {
    return globalAllocations().load(std::memory_order_relaxed);
  }
  static uint64_t totalBytes() noexcept { return globalBytes().load(std::memory_order_relaxed); }

 private:
  void* do_allocate(size_t bytes, size_t align) override
  {
    _allocations.fetch_add(1, std::memory_order_relaxed);
    _bytes.fetch_add(bytes, std::memory_order_relaxed);
    globalAllocations().fetch_add(1, std::memory_order_relaxed);
    globalBytes().fetch_add(bytes, std::memory_order_relaxed);
    return _upstream->allocate(bytes, align);
  }

  void do_deallocate(void* p, size_t bytes, size_t align) override
  {
    _upstream->deallocate(p, bytes, align);
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
  {
    return this == &other;
  }

  static std::atomic<uint64_t>& globalAllocations()
  {
    static std::atomic<uint64_t> v{0};
    return v;
  }
  static std::atomic<uint64_t>& globalBytes()
  {
    static std::atomic<uint64_t> v{0};
    return v;
  }

  std::pmr::memory_resource* _upstream;
  std::atomic<uint64_t> _allocations{0};
  std::atomic<uint64_t> _bytes{0};
};

}  // namespace flox::memory
