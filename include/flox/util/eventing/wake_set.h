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
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace flox
{

// One wait point shared by many rings.
//
// A parked consumer waits on the condition variable of its own bus, which is
// the right shape while a consumer IS a thread. A thread that steps hundreds
// of consumers across dozens of buses has no such bus: it cannot block on any
// one of them without going deaf to the rest, so until now it had no choice
// but to spin, and spinning is what parking was built to stop.
//
// A WakeSet is the missing wait point. The stepping thread sleeps on the set;
// every bus it steps is told about the set and wakes it on publish. One
// condition variable, one mutex, one waiter count, however many rings hang
// off it.
//
// The discipline is the one park() already uses, for the same reason: the
// window between a waiter's last look at its rings and it raising its hand is
// where a wake-up gets lost. The count goes up BEFORE the look, with a
// sequentially consistent fence on either side (here and in wake()), so a
// publisher that misses the count cannot also be missed by the look; and the
// mutex is held across both the look and the wait, so a publisher that sees
// the count cannot slip its notify into the gap. The timed wait underneath is
// a net, not the mechanism -- a missed wake-up costs one interval instead of
// forever.
//
// Not thread-affine and not per bus: several threads may park on one set, and
// any of them may be the one woken. What it is not is a scheduler -- it says
// "something happened somewhere", and the waiter looks again.
class WakeSet
{
 public:
  using Probe = void (*)(void*);

  // A missed wake-up costs one of these, not forever. Long on purpose, and
  // the same interval a bus uses for its own parked consumers: it is a net,
  // and a net that catches things often is a mechanism nobody meant to build.
  static constexpr auto kNetInterval = std::chrono::milliseconds(50);

  WakeSet() = default;
  WakeSet(const WakeSet&) = delete;
  WakeSet& operator=(const WakeSet&) = delete;

  // Test seam for the one race the set has to survive: a publish landing
  // between the waiter's last look at its rings and it raising its hand. That
  // window is a couple of hundred nanoseconds wide and sits inside
  // parkUnless(), so no test can aim at it from outside -- and an untestable
  // correctness claim is a claim nobody checks. The hook is called at exactly
  // that instant, and a test publishes from it.
  //
  // Null by default: one predictable branch on the way into a sleep, nothing
  // at all anywhere else.
  void setProbe(Probe probe, void* user) noexcept
  {
    _probe = probe;
    _probeUser = user;
  }

  // Sleep until somebody wakes the set, unless hasWork() says there is
  // something to do. hasWork() is the caller's last look at every ring it
  // steps, and it is evaluated inside the set's discipline -- which is the
  // whole point, and why this is not "check, then call park()".
  //
  // It must be cheap and must not deliver anything: it runs under the set's
  // mutex, and a handler called from there would hold up every publisher
  // trying to wake the set.
  template <typename Pred>
  void parkUnless(Pred&& hasWork)
  {
    if (_probe != nullptr)
    {
      // Before the mutex and before the hand goes up: the exact instant a
      // publisher can miss this waiter. See setProbe().
      _probe(_probeUser);
    }
    std::unique_lock<std::mutex> lk(_mx);
    _waiters.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!hasWork())
    {
      _cv.wait_for(lk, kNetInterval);
    }
    _waiters.fetch_sub(1, std::memory_order_relaxed);
  }

  // Wake whoever is parked on the set. Called by a bus after a publish and on
  // its way down, and callable by anything else that changed the world the
  // waiter's predicate looks at.
  void wake()
  {
    // The fence pairs with the one in parkUnless(): between them, a publisher
    // that reads no waiters is guaranteed to have made its event visible to a
    // waiter that is about to take its last look.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (_waiters.load(std::memory_order_seq_cst) == 0)
    {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(_mx);
    }
    _cv.notify_all();
  }

  // Advisory: how many threads are asleep on the set right now.
  uint32_t waiters() const noexcept { return _waiters.load(std::memory_order_acquire); }

 private:
  alignas(64) std::atomic<uint32_t> _waiters{0};
  Probe _probe{nullptr};
  void* _probeUser{nullptr};
  std::mutex _mx;
  std::condition_variable _cv;
};

}  // namespace flox
