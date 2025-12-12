/*
 * FLOX Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <thread>
#include <type_traits>
#include <utility>

// Check if std::jthread is available
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
#define FLOX_HAS_STD_JTHREAD 1
#else
#define FLOX_HAS_STD_JTHREAD 0
#endif

namespace flox
{

#if FLOX_HAS_STD_JTHREAD

using jthread = std::jthread;

#else

/**
 * Minimal jthread implementation for platforms without C++20 std::jthread.
 * This implementation provides automatic join on destruction but does NOT
 * support stop_token/stop_source functionality.
 */
class jthread
{
 public:
  jthread() noexcept = default;

  template <typename F, typename... Args,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, jthread>>>
  explicit jthread(F&& f, Args&&... args)
      : _thread(std::forward<F>(f), std::forward<Args>(args)...)
  {
  }

  ~jthread()
  {
    if (_thread.joinable())
    {
      _thread.join();
    }
  }

  jthread(const jthread&) = delete;
  jthread& operator=(const jthread&) = delete;

  jthread(jthread&& other) noexcept : _thread(std::move(other._thread)) {}

  jthread& operator=(jthread&& other) noexcept
  {
    if (this != &other)
    {
      if (_thread.joinable())
      {
        _thread.join();
      }
      _thread = std::move(other._thread);
    }
    return *this;
  }

  bool joinable() const noexcept { return _thread.joinable(); }

  void join()
  {
    if (_thread.joinable())
    {
      _thread.join();
    }
  }

  void detach() { _thread.detach(); }

  std::thread::id get_id() const noexcept { return _thread.get_id(); }

  std::thread::native_handle_type native_handle() { return _thread.native_handle(); }

  static unsigned int hardware_concurrency() noexcept
  {
    return std::thread::hardware_concurrency();
  }

 private:
  std::thread _thread;
};

#endif

}  // namespace flox
