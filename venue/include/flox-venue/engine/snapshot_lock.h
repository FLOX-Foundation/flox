/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The lock between the matching consumer and whoever samples it.
 *
 * A few of the engine's containers exist to be read from somewhere else:
 * docs/venue/perimeter.md tells a deployment to read the last-look stats, the
 * admission profiles and the resting-order count off the engine and hand them
 * to prom::render, and MetricsServer runs that sampler on the connection
 * thread that answered the scrape. Everything else in the engine belongs to
 * the consumer thread alone and is read by nobody.
 *
 * WHY A SPINLOCK AND NOT A MUTEX. The consumer thread is the matching path;
 * what it pays here it pays on every order that touches the guarded
 * container, and a scrape happens once every few seconds. Uncontended, this
 * is one atomic exchange and one store -- no syscall, no allocation, no
 * fairness bookkeeping -- and uncontended is the case 99.999% of the time.
 * Contended, the holder is a reader copying a small map, which is bounded
 * work with no I/O and no allocation the consumer waits on indefinitely; the
 * spin yields after a short burst so a preempted reader cannot burn the
 * consumer's quantum. A std::mutex would be correct too and is the wrong
 * shape: it optimises for long critical sections and contention, which is
 * exactly what this is not.
 *
 * WHAT IT DOES NOT GUARD. Per-order bookkeeping -- the resting-order index --
 * is not behind this: one lock per resting order is a cost the matching path
 * must not pay for an observability gauge, so that count is published through
 * an atomic instead (engine::Publications). This lock is for the containers a
 * sampler needs a COHERENT COPY of, where an atomic per field would not help:
 * a row of LastLookStats that is read between the `held` write and the
 * accepted/rejected write is a row that does not balance, and a page built
 * from it is a scrape of no moment at all.
 */
#pragma once

#include <atomic>
#include <thread>

namespace flox::venue::engine
{

class SnapshotLock
{
 public:
  SnapshotLock() = default;
  SnapshotLock(const SnapshotLock&) = delete;
  SnapshotLock& operator=(const SnapshotLock&) = delete;

  void lock() noexcept
  {
    unsigned spins = 0;
    while (held_.exchange(true, std::memory_order_acquire))
    {
      // Read-only until it looks free: a test-and-test-and-set keeps the
      // waiter off the cache line the holder is writing.
      while (held_.load(std::memory_order_relaxed))
      {
        if (++spins < kSpinsBeforeYield)
        {
          continue;
        }
        spins = 0;
        // The holder may have been descheduled mid-copy. Spinning through
        // that would cost the consumer a whole quantum for a scrape.
        std::this_thread::yield();
      }
    }
  }

  void unlock() noexcept { held_.store(false, std::memory_order_release); }

 private:
  static constexpr unsigned kSpinsBeforeYield = 64;

  alignas(64) std::atomic<bool> held_{false};
};

}  // namespace flox::venue::engine
