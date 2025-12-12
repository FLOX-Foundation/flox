/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define FLOX_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#define FLOX_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define FLOX_CPU_PAUSE() ((void)0)
#endif

namespace flox
{

struct BusyBackoff
{
  int spins = 0;

  inline void pause()
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

  inline void reset() { spins = 0; }
};

}  // namespace flox
