/*
 * FLOX Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace flox::venue
{

// A checkpoint stops a shard: matching is paused while the book is cloned and
// the journal rotated, and the snapshot is written in the background after
// that. Measured at 221 ms on a 100k-order book.
//
// While a shard owns a thread, that pause costs exactly one symbol, which is
// the price of a deterministic snapshot and was always meant to be paid. Once
// one thread drives many shards it costs all of them -- and the automatic
// trigger is the current segment's size, so equally loaded shards reach it
// together. The pause multiplies exactly when the load is even.
//
// The lane is the permission to take that pause, held from the start of the
// pause until the background snapshot has been written. At most one shard on
// a lane holds it, so:
//
//   - two shards sharing a driver never pause at the same time;
//   - at most one snapshot per lane is being written to disk at a time, which
//     also caps the threads the publishes spawn.
//
// A shard that cannot get the lane SKIPS its automatic checkpoint and asks
// again at the next command boundary -- waiting for it would be the pause it
// is trying to avoid. A checkpoint asked for by name (checkpointNow) waits
// instead, because somebody is waiting for the answer.
//
// Shards on different lanes are independent; a shard with no lane behaves
// exactly as before.
class CheckpointLane
{
 public:
  bool tryEnter() noexcept
  {
    bool expected = false;
    return busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                         std::memory_order_relaxed);
  }

  // For the path that must not be skipped. Spins with a yield: the lane is
  // held for the length of one snapshot publish, and this runs on an
  // operator's call, not on the matching path.
  void enterBlocking() noexcept
  {
    while (!tryEnter())
    {
      std::this_thread::yield();
    }
  }

  void leave() noexcept { busy_.store(false, std::memory_order_release); }

  bool busy() const noexcept { return busy_.load(std::memory_order_acquire); }

  // The pause is a property of the lane, not of any one shard on it: what an
  // operator needs to know is how long the driver was stopped, by anybody.
  void notePause(int64_t ns) noexcept
  {
    pauseTotalNs_.fetch_add(ns, std::memory_order_relaxed);
    int64_t prev = pauseMaxNs_.load(std::memory_order_relaxed);
    while (ns > prev && !pauseMaxNs_.compare_exchange_weak(prev, ns, std::memory_order_relaxed))
    {
    }
    checkpoints_.fetch_add(1, std::memory_order_relaxed);
  }

  void noteSkipped() noexcept { skipped_.fetch_add(1, std::memory_order_relaxed); }

  int64_t pauseTotalNs() const noexcept { return pauseTotalNs_.load(std::memory_order_acquire); }
  int64_t pauseMaxNs() const noexcept { return pauseMaxNs_.load(std::memory_order_acquire); }
  uint64_t checkpoints() const noexcept { return checkpoints_.load(std::memory_order_acquire); }
  // Automatic checkpoints that found the lane taken. Not an error: the shard
  // asks again at its next boundary. A number that keeps climbing on one shard
  // while others checkpoint fine is a shard being crowded out, and that is
  // what this counter is for.
  uint64_t skipped() const noexcept { return skipped_.load(std::memory_order_acquire); }

 private:
  alignas(64) std::atomic<bool> busy_{false};
  std::atomic<int64_t> pauseTotalNs_{0};
  std::atomic<int64_t> pauseMaxNs_{0};
  std::atomic<uint64_t> checkpoints_{0};
  std::atomic<uint64_t> skipped_{0};
};

}  // namespace flox::venue
