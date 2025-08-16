/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <thread>
#if defined(__x86_64__) || defined(__aarch64__)
#include <immintrin.h>
#endif

namespace flox
{

struct BusyBackoff
{
  int spins = 0;

  inline void pause()
  {
    if (spins < 64)
    {
#if defined(__x86_64__) || defined(__aarch64__)
      _mm_pause();
#endif
    }
    else if (spins < 256)
    {
      std::this_thread::yield();
    }
    else
    {
      struct timespec ts
      {
        0, 50000
      };
      nanosleep(&ts, nullptr);
      spins = 0;
      return;
    }

    ++spins;
  }

  inline void reset() { spins = 0; }
};

}  // namespace flox
