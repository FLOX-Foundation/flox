/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace flox::memory
{

// Lock-free freelist over a fixed slot array, safe for push and pop from any
// thread.
//
// A pool cannot use an SPSC queue for its freelist. A bus slot owns its Handle
// until the slot is overwritten, and the overwrite -- destructor included --
// runs on whichever thread is publishing, so with several connectors sharing a
// bus an event is returned to its pool by a thread that does not own that pool.
// Several producers, therefore, and an SPSC queue is undefined behaviour there.
//
// Slots are addressed by 32-bit index rather than by pointer, so the head fits
// in a single 64-bit word as (tag << 32) | index. The tag advances on every
// successful update, which is what keeps ABA away: a slot that leaves and comes
// back between a read and its compare-exchange arrives under a different tag,
// so the exchange fails and retries instead of splicing a stale successor.
// A 32-bit tag wraps only after 4 billion updates of the same word, and a
// wrap is harmless unless it lands inside one thread's read/CAS window.
//
// Order is LIFO: the most recently released slot is handed out next, which is
// also the one most likely to still be in cache.
template <size_t Capacity>
class IndexFreelist
{
  static_assert(Capacity < 0xFFFFFFFFu, "Capacity must fit in a 32-bit index");

 public:
  static constexpr uint32_t kNull = 0xFFFFFFFFu;

  IndexFreelist() { _head.store(pack(0, kNull), std::memory_order_relaxed); }

  void push(uint32_t index) noexcept
  {
    uint64_t head = _head.load(std::memory_order_relaxed);
    uint64_t next;
    do
    {
      _next[index].store(indexOf(head), std::memory_order_relaxed);
      next = pack(tagOf(head) + 1, index);
    } while (!_head.compare_exchange_weak(head, next, std::memory_order_release,
                                          std::memory_order_relaxed));
  }

  // Returns kNull when the list is empty.
  uint32_t pop() noexcept
  {
    uint64_t head = _head.load(std::memory_order_acquire);
    for (;;)
    {
      const uint32_t index = indexOf(head);
      if (index == kNull)
      {
        return kNull;
      }
      const uint64_t next = pack(tagOf(head) + 1, _next[index].load(std::memory_order_relaxed));
      if (_head.compare_exchange_weak(head, next, std::memory_order_acquire,
                                      std::memory_order_acquire))
      {
        return index;
      }
    }
  }

 private:
  static constexpr uint64_t pack(uint32_t tag, uint32_t index)
  {
    return (static_cast<uint64_t>(tag) << 32) | index;
  }
  static constexpr uint32_t tagOf(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
  static constexpr uint32_t indexOf(uint64_t v) { return static_cast<uint32_t>(v); }

  alignas(64) std::atomic<uint64_t> _head{};
  std::array<std::atomic<uint32_t>, Capacity> _next{};
};

}  // namespace flox::memory
