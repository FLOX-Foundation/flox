/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/util/concurrency/spsc_queue.h"
#include "flox/util/memory/ref_countable.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <type_traits>

namespace flox::concepts
{
template <typename T>
concept Poolable = RefCountable<T> && requires(T* obj) {
  { obj->setPool(nullptr) } -> std::same_as<void>;
  { obj->releaseToPool() } -> std::same_as<void>;
  { obj->clear() } -> std::same_as<void>;
};
}  // namespace flox::concepts

namespace flox::pool
{

template <typename Derived>
struct PoolableBase : public RefCountable
{
  void* _origin = nullptr;

  void setPool(void* pool) { _origin = pool; }

  void releaseToPool()
  {
    assert(_origin && _releaseFn && "Pool or releaseFn not set");
    _releaseFn(_origin, static_cast<Derived*>(this));
  }

  static inline void (*_releaseFn)(void*, void*) = nullptr;

  void clear() {}
};

template <typename T>
class Handle
{
  static_assert(concepts::RefCountable<T>, "T must be RefCountable");

 public:
  Handle() = delete;

  explicit Handle(T* ptr) noexcept : _ptr(ptr)
  {
    assert(ptr != nullptr);
    retain(_ptr);
  }

  Handle(const Handle& other) noexcept : _ptr(other._ptr)
  {
    retain(_ptr);
  }

  Handle& operator=(const Handle&) = delete;

  Handle(Handle&& other) noexcept : _ptr(other._ptr)
  {
    other._ptr = nullptr;
  }

  Handle& operator=(Handle&& other) noexcept
  {
    if (this != &other)
    {
      release();
      _ptr = other._ptr;
      other._ptr = nullptr;
    }
    return *this;
  }

  ~Handle()
  {
    release();
  }

  T* get() const noexcept { return _ptr; }
  T* operator->() const noexcept { return _ptr; }
  T& operator*() const noexcept { return *_ptr; }

  template <typename U>
  Handle<U> upcast() const
  {
    static_assert(std::is_base_of_v<U, T>);
    retain(_ptr);
    return Handle<U>(_ptr);
  }

 private:
  T* _ptr;

  static void retain(T* e)
  {
    e->retain();
  }

  static void release(T* e)
  {
    if (e && e->release())
    {
      e->releaseToPool();
    }
  }

  void release()
  {
    release(_ptr);
    _ptr = nullptr;
  }
};

template <typename T, size_t Capacity>
class Pool
{
  static_assert(concepts::RefCountable<T>, "T must be RefCountable");
  static_assert(concepts::Poolable<T>, "T must be Poolable");

 public:
  using ObjectType = T;

  Pool()
      : _arena(_buffer.data(), _buffer.size()),
        _pool(&_arena)
  {
    for (size_t i = 0; i < Capacity; ++i)
    {
      auto* obj = new (&_slots[i]) T(&_pool);

      obj->setPool(this);

      T::_releaseFn = [](void* pool, void* ptr)
      {
        static_cast<Pool<T, Capacity>*>(pool)->release(static_cast<T*>(ptr));
      };

      _queue.push(obj);
    }
  }

  ~Pool()
  {
    T::_releaseFn = nullptr;
  }

  std::optional<Handle<T>> acquire()
  {
    T* obj = nullptr;
    if (_queue.pop(obj))
    {
      obj->resetRefCount();
      obj->setPool(this);

      ++_acquired;
      return Handle<T>(obj);
    }

    return std::nullopt;
  }

  void release(T* obj)
  {
    obj->clear();
    _queue.push(obj);
    ++_released;
  }

  size_t inUse() const { return _acquired - _released; }

 private:
  using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
  Storage _slots[Capacity];

  std::array<std::byte, 128 * 1024> _buffer;
  std::pmr::monotonic_buffer_resource _arena;
  std::pmr::unsynchronized_pool_resource _pool;

  SPSCQueue<T*, Capacity + 1> _queue;

  size_t _acquired = 0;
  size_t _released = 0;
};

}  // namespace flox::pool