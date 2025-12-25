/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <chrono>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define FLOX_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#define FLOX_CPU_PAUSE() __asm__ __volatile__("yield" :: \
                                                  : "memory")
#else
#define FLOX_CPU_PAUSE() ((void)0)
#endif

namespace flox
{

enum class BackoffMode
{
  AGGRESSIVE,  ///< Dedicated colo: busy-spin with CPU pause, minimal yields
  RELAXED,     ///< Shared VPS/cloud: early sleep, minimal CPU burn
  ADAPTIVE     ///< Auto-adjust: starts aggressive, backs off under contention
};

namespace config
{
inline BackoffMode defaultBackoffMode = BackoffMode::ADAPTIVE;
}  // namespace config

struct BusyBackoff
{
  int spins = 0;
  BackoffMode mode;

  explicit BusyBackoff(BackoffMode m = config::defaultBackoffMode) : mode(m) {}

  inline void pause()
  {
    switch (mode)
    {
      case BackoffMode::AGGRESSIVE:
        pauseAggressive();
        break;
      case BackoffMode::RELAXED:
        pauseRelaxed();
        break;
      case BackoffMode::ADAPTIVE:
        pauseAdaptive();
        break;
    }
  }

  inline void reset() { spins = 0; }

 private:
  inline void pauseAggressive()
  {
    if (spins < 2048)
    {
      FLOX_CPU_PAUSE();
      ++spins;
      return;
    }

    std::this_thread::yield();
    if (spins < 4096)
    {
      ++spins;
    }
    else
    {
      spins = 0;
    }
  }

  inline void pauseRelaxed()
  {
    if (spins < 8)
    {
      // Brief spin for immediate data
      FLOX_CPU_PAUSE();
      ++spins;
      return;
    }

    if (spins < 16)
    {
      // Quick yield
      std::this_thread::yield();
      ++spins;
      return;
    }

    // Sleep to release CPU - 100us is enough for market data which arrives
    // at most every few milliseconds per symbol
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (spins < 64)
    {
      ++spins;
    }
    else
    {
      // Increase sleep duration for sustained idle periods
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  }

  inline void pauseAdaptive()
  {
    if (spins < 128)
    {
      // Initial aggressive phase for low-latency burst handling
      FLOX_CPU_PAUSE();
      ++spins;
      return;
    }

    if (spins < 512)
    {
      // Medium contention: yield to other threads
      std::this_thread::yield();
      ++spins;
      return;
    }

    if (spins < 2048)
    {
      // High contention: short sleep
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      ++spins;
      return;
    }

    // Sustained contention: longer sleep, reset cycle
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    spins = 512;  // Reset to medium level, not zero
  }
};

}  // namespace flox
