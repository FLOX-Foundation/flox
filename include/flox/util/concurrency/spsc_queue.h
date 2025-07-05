/*
 * Flox Engine
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace flox
{

template <typename T, size_t Capacity>
class SPSCQueue
{
  static_assert(Capacity > 0, "Capacity must be > 0");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");

 public:
  SPSCQueue() = default;

  ~SPSCQueue()
  {
    while (!empty())
    {
      if (T* ptr = try_pop())
      {
        ptr->~T();
      }
    }
  }

  bool push(const T& item) noexcept
  {
    const size_t head = _head.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & MASK;

    if (next == _tail.load(std::memory_order_acquire))
      return false;

    new (static_cast<void*>(&_buffer[head])) T(item);
    _head.store(next, std::memory_order_release);
    return true;
  }

  bool emplace(T&& item) noexcept
  {
    const size_t head = _head.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & MASK;

    if (next == _tail.load(std::memory_order_acquire))
      return false;

    new (static_cast<void*>(&_buffer[head])) T(std::move(item));
    _head.store(next, std::memory_order_release);
    return true;
  }

  template <typename... Args>
  bool try_emplace(Args&&... args)
  {
    const size_t head = _head.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & MASK;
    if (next == _tail.load(std::memory_order_acquire))
      return false;

    void* ptr = static_cast<void*>(&_buffer[head]);
    new (ptr) T(std::forward<Args>(args)...);

    _head.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T& out) noexcept
  {
    const size_t tail = _tail.load(std::memory_order_relaxed);
    if (tail == _head.load(std::memory_order_acquire))
    {
      return false;
    }

    T* ptr = reinterpret_cast<T*>(&_buffer[tail]);
    out = std::move(*ptr);
    ptr->~T();

    const size_t next = (tail + 1) & MASK;
    _tail.store(next, std::memory_order_release);
    return true;
  }

  T* try_pop()
  {
    const size_t tail = _tail.load(std::memory_order_relaxed);
    if (tail == _head.load(std::memory_order_acquire))
      return nullptr;

    T* ptr = reinterpret_cast<T*>(&_buffer[tail]);
    const size_t next = (tail + 1) & MASK;
    _tail.store(next, std::memory_order_release);

    return ptr;
  }

  std::optional<std::reference_wrapper<T>> try_pop_ref()
  {
    const size_t tail = _tail.load(std::memory_order_relaxed);
    if (tail == _head.load(std::memory_order_acquire))
      return std::nullopt;

    T* ptr = reinterpret_cast<T*>(&_buffer[tail]);
    const size_t next = (tail + 1) & MASK;
    _tail.store(next, std::memory_order_release);

    return std::ref(*ptr);
  }

  void clear() noexcept
  {
    while (!empty())
    {
      const size_t tail = _tail.load(std::memory_order_relaxed);
      T* ptr = reinterpret_cast<T*>(&_buffer[tail]);
      ptr->~T();
      const size_t next = (tail + 1) & MASK;
      _tail.store(next, std::memory_order_release);
    }
  }

  bool empty() const noexcept
  {
    return _head.load(std::memory_order_acquire) == _tail.load(std::memory_order_acquire);
  }

  bool full() const noexcept
  {
    const size_t next = (_head.load(std::memory_order_acquire) + 1) & MASK;
    return next == _tail.load(std::memory_order_acquire);
  }

  size_t size() const noexcept
  {
    const size_t head = _head.load(std::memory_order_acquire);
    const size_t tail = _tail.load(std::memory_order_acquire);
    return (head + Capacity - tail) & MASK;
  }

 private:
  alignas(64) std::atomic<size_t> _head{0};
  alignas(64) std::atomic<size_t> _tail{0};

  static constexpr size_t MASK = Capacity - 1;

  using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
  alignas(64) Storage _buffer[Capacity];
};

}  // namespace flox
