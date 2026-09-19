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
#include <cstdio>
#include <exception>
#include <thread>
#include <utility>

namespace flox
{

// An exception leaving a thread body is std::terminate for the whole process:
// no unwinding, no message, every engine in the process gone because one
// connection parsed badly or one log rotation found its directory missing.
// Nothing further up can catch it, because there is nothing further up -- the
// thread body IS the top of its stack. The only place that can contain it is
// the body itself.
//
// Containment alone is not enough. A thread that stops silently is its own
// kind of failure: a sweeper that no longer checkpoints, a writer that no
// longer writes, a feed that no longer feeds. That failure is quieter than
// the crash it replaced and therefore harder to find. So a contained death is
// reported on stderr and counted, and the count is readable at runtime by
// whoever supervises the process.
namespace detail
{
inline std::atomic<uint64_t> gThreadDeaths{0};
// The name is always a string literal owned by the caller, so the pointer
// outlives every reader. The exception message is not -- it is printed and
// dropped rather than copied, because copying it would allocate on the one
// path where allocation is most likely to be what failed.
inline std::atomic<const char*> gLastDeadThread{nullptr};

inline void noteThreadDeath(const char* name, const char* what) noexcept
{
  gLastDeadThread.store(name, std::memory_order_release);
  gThreadDeaths.fetch_add(1, std::memory_order_release);
  std::fprintf(stderr, "flox: thread '%s' died on an exception, contained: %s\n",
               name ? name : "?", what ? what : "?");
}
}  // namespace detail

// How many thread bodies have died on an exception since the process started.
inline uint64_t threadDeathCount() noexcept
{
  return detail::gThreadDeaths.load(std::memory_order_acquire);
}

// The name of the most recent such thread, or nullptr if there has been none.
inline const char* lastDeadThreadName() noexcept
{
  return detail::gLastDeadThread.load(std::memory_order_acquire);
}

// Run fn as the body of a thread. Every std::thread in the framework goes
// through here; see the note above for why.
//
// `name` must be a string literal: it is stored, not copied.
//
// Returns true if the body returned normally, false if it died. Callers use
// that to release whatever was waiting on the thread.
template <typename Fn>
bool runThreadBody(const char* name, Fn&& fn) noexcept
{
  try
  {
    std::forward<Fn>(fn)();
    return true;
  }
  catch (const std::exception& e)
  {
    detail::noteThreadDeath(name, e.what());
  }
  catch (...)
  {
    detail::noteThreadDeath(name, "unknown exception");
  }
  return false;
}

// Construct a thread whose body is already contained. Prefer this over
// std::thread directly: it keeps the guarantee at the point where the thread
// is created rather than relying on every body to remember it.
template <typename Fn>
std::thread makeThread(const char* name, Fn&& fn)
{
  return std::thread([name, f = std::forward<Fn>(fn)]() mutable
                     { runThreadBody(name, f); });
}

// As above, plus a release to run if -- and only if -- the body died.
//
// Containment on its own can turn a crash into a deadlock, which for a
// process holding positions is the worse of the two: whoever was parked
// waiting for this thread to make progress now waits for a thread that is no
// longer there. Components with such a waiter pass a release that does what
// their shutdown path does -- drop the running flag, wake the condition
// variables, break the promises -- so the wait ends rather than hangs.
//
// The release is contained too: a release that throws would terminate the
// process on the one path whose whole purpose is to prevent that.
template <typename Fn, typename OnDeath>
std::thread makeThread(const char* name, Fn&& fn, OnDeath&& onDeath)
{
  return std::thread(
      [name, f = std::forward<Fn>(fn), d = std::forward<OnDeath>(onDeath)]() mutable
      {
        if (!runThreadBody(name, f))
        {
          runThreadBody(name, d);
        }
      });
}

}  // namespace flox
