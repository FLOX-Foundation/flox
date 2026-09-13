/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/util/memory/counting_resource.h"
#include "flox/util/memory/freelist.h"
#include "flox/util/memory/ref_countable.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

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

// Type-erased view of the pool an object came from, so a pooled object can
// name its owner without knowing the pool's Capacity.
//
// The release path used to go through a function pointer held as a static
// member of the object type, which made it one pointer for every pool of that
// type rather than one per pool. Two pools of the same T -- two connectors
// each holding a Pool<BookUpdateEvent, N> is the ordinary case -- then shared
// it: the second pool's constructor rewrote it to a lambda that casts the
// pool pointer to its own Capacity, so releasing an object into the first
// pool wrote the freelist and the counters at the second pool's offsets,
// landing inside the first pool's live slot array. Destroying either pool
// nulled it for both, and the next release dereferenced null.
//
// The owner is per object now, set when the pool constructs the slot and
// stored in the same pointer-sized field that used to hold the raw pool
// address, so no pooled type changes size or layout.
struct PoolReleaser
{
  virtual void releaseErased(void* obj) = 0;

 protected:
  ~PoolReleaser() = default;
};

template <typename Derived>
struct PoolableBase : public RefCountable
{
  PoolReleaser* _origin = nullptr;

  void setPool(PoolReleaser* pool) { _origin = pool; }

  void releaseToPool()
  {
    assert(_origin && "Pool not set");
    _origin->releaseErased(static_cast<Derived*>(this));
  }

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
    // Handle's constructor retains; retaining here as well would leak a
    // reference and eventually starve the pool.
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
class Pool final : public PoolReleaser
{
  static_assert(concepts::RefCountable<T>, "T must be RefCountable");
  static_assert(concepts::Poolable<T>, "T must be Poolable");

 public:
  using ObjectType = T;

  Pool()
      : _arena(_buffer.data(), _buffer.size(), &_upstream),
        _pool(&_arena)
  {
    for (size_t i = 0; i < Capacity; ++i)
    {
      auto* obj = new (&_slots[i]) T(&_pool);

      obj->setPool(this);

      _freelist.push(static_cast<uint32_t>(i));
    }
  }

  // Nothing to unwind: the owner pointer each object carries dies with the
  // slot it points into, so destroying one pool cannot disturb another.
  ~Pool() = default;

  void releaseErased(void* obj) override { release(static_cast<T*>(obj)); }

  using ExhaustionCallback = void (*)(size_t capacity, size_t inUse);

  void setExhaustionCallback(ExhaustionCallback cb) { _exhaustionCb = cb; }

  // Grow every pooled object to the size the traffic will actually need,
  // at startup. The pmr vectors inside pooled events keep their capacity
  // across clear(), so reserving here means the steady state never touches
  // the arena -- and, more importantly, that the arena's fall back to the
  // heap happens now instead of on the first deep snapshot mid-session.
  //
  //   pool.prewarm([](BookUpdateEvent& e) {
  //     e.update.bids.reserve(maxDepth);
  //     e.update.asks.reserve(maxDepth);
  //   });
  //
  // Check upstreamAllocations() afterwards: non-zero means the inline
  // buffer is too small for this configuration and the pool is holding
  // heap memory it will never give back.
  template <typename Fn>
  void prewarm(Fn&& fn)
  {
    for (size_t i = 0; i < Capacity; ++i)
    {
      fn(*std::launder(reinterpret_cast<T*>(&_slots[i])));
    }
  }

  // Allocations that escaped the inline buffer into the heap. Zero is the
  // healthy value; see prewarm().
  uint64_t upstreamAllocations() const noexcept { return _upstream.allocations(); }
  uint64_t upstreamBytes() const noexcept { return _upstream.bytes(); }

  // Appends up to `count` Handles to `out` and returns how many it got.
  // Fewer than `count` means the pool ran dry.
  size_t acquireBatch(std::vector<Handle<T>>& out, size_t count)
  {
    size_t got = 0;
    while (got < count)
    {
      T* obj = popSlot();
      if (!obj)
      {
        break;
      }
      out.emplace_back(obj);
      ++got;
    }
    _acquired.fetch_add(got, std::memory_order_relaxed);
    if (got < count)
    {
      _exhaustionCount.fetch_add(1, std::memory_order_relaxed);
      if (_exhaustionCb)
      {
        _exhaustionCb(Capacity, inUse());
      }
    }
    return got;
  }

  std::optional<Handle<T>> acquire()
  {
    if (T* obj = popSlot())
    {
      _acquired.fetch_add(1, std::memory_order_relaxed);
      return Handle<T>(obj);
    }

    _exhaustionCount.fetch_add(1, std::memory_order_relaxed);
    if (_exhaustionCb)
    {
      _exhaustionCb(Capacity, inUse());
    }

    return std::nullopt;
  }

  // Called from whichever thread drops the last reference, which is not
  // necessarily the thread that acquired the object: a bus slot is destroyed
  // by the publisher that overwrites it, so with several connectors on one bus
  // an event returns to its pool from a foreign thread. The freelist and these
  // counters are therefore all multi-producer safe.
  void release(T* obj)
  {
    obj->clear();
    _freelist.push(indexOf(obj));
    _released.fetch_add(1, std::memory_order_relaxed);
  }

  size_t inUse() const
  {
    const size_t acquired = _acquired.load(std::memory_order_relaxed);
    const size_t released = _released.load(std::memory_order_relaxed);
    return acquired - released;
  }
  size_t capacity() const { return Capacity; }
  size_t exhaustionCount() const { return _exhaustionCount.load(std::memory_order_relaxed); }
  size_t acquireCount() const { return _acquired.load(std::memory_order_relaxed); }
  size_t releaseCount() const { return _released.load(std::memory_order_relaxed); }

 private:
  uint32_t indexOf(const T* obj) const noexcept
  {
    return static_cast<uint32_t>(reinterpret_cast<const Storage*>(obj) - &_slots[0]);
  }

  T* popSlot() noexcept
  {
    const uint32_t index = _freelist.pop();
    if (index == memory::IndexFreelist<Capacity>::kNull)
    {
      return nullptr;
    }
    T* obj = std::launder(reinterpret_cast<T*>(&_slots[index]));
    obj->resetRefCount();
    obj->setPool(this);
    return obj;
  }

  struct alignas(alignof(T)) Storage
  {
    std::byte data[sizeof(T)];
  };
  Storage _slots[Capacity];

  std::array<std::byte, 128 * 1024> _buffer;
  memory::CountingResource _upstream;
  std::pmr::monotonic_buffer_resource _arena;
  // Synchronized, not unsynchronized: pooled events are filled by whichever
  // thread acquired them, so two publishers growing their own event's vectors
  // hit this one resource concurrently -- with the unsynchronized variant they
  // could be handed the same block. The mutex is only touched when a vector
  // actually grows; after prewarm() the steady state never allocates, so the
  // hot path does not pay for it. Every call the pool makes into _arena also
  // happens under that mutex, which is what makes the non-thread-safe
  // monotonic arena underneath safe to share.
  std::pmr::synchronized_pool_resource _pool;

  memory::IndexFreelist<Capacity> _freelist;

  std::atomic<size_t> _acquired{0};
  std::atomic<size_t> _released{0};
  std::atomic<size_t> _exhaustionCount{0};
  ExhaustionCallback _exhaustionCb = nullptr;
};

}  // namespace flox::pool