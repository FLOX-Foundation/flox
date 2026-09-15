/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// src/capi/tracker_ownership.h -- one ownership rule for every optional
// bridge hook reachable from the engine's signal-dispatch threads (risk
// manager, kill switch, order validator, PnL tracker, storage sink,
// executor).
//
// Each hook is created and destroyed through a C handle that used to be
// nothing more than a pointer value: flox_pnl_tracker_create() handed back
// the address of a heap object, and flox_pnl_tracker_destroy() called
// `delete` on it directly. That is only safe if nothing else can still be
// dereferencing the address at the moment of the delete. Nothing enforced
// that. The engine's bus consumer threads read whatever pointer was last
// handed to RunnerSignalHandler and call straight through it, and a call
// into a binding language's callback can run for an arbitrary amount of
// time. A caller doing the documented right thing --
// flox_runner_set_pnl_tracker(runner, NULL) followed by
// flox_pnl_tracker_destroy(tracker) -- only stops *future* dispatches from
// picking up the old pointer. A dispatch already past the load, mid-way
// through the user's on_signal callback with the old pointer in hand, has
// no way to know the object underneath it is about to disappear, and no
// amount of reordering those two calls closes the window.
//
// The fix is shared ownership in place of a bare pointer, so the object
// stays alive for as long as anything might still be using it:
//
//  * TrackerRegistry<T> is where create()/destroy() live. create() builds
//    the object with std::make_shared and keeps the master reference;
//    destroy() drops that reference. The C handle is still a plain T* --
//    binding code, and the backtest path (which never races the live
//    engine's dispatch threads), keep working unchanged.
//
//  * TrackerSlot<T> is what RunnerSignalHandler actually holds for each
//    hook. set() stores a strong reference, looked up in the registry by
//    the handle's address; a dispatch thread calls get() once per signal
//    and keeps the returned shared_ptr for the duration of the callback.
//    Even if destroy() runs on another thread at that exact moment and
//    drops the registry's reference, the copy the dispatch thread is
//    holding keeps the object alive until the callback returns and that
//    copy goes out of scope.
//
// Net effect: destroy() is safe to call the instant set(NULL) returns, on
// any thread, regardless of what the engine's dispatch threads are doing
// at that moment. The object is freed once every reference -- the
// registry's, and any snapshot a dispatch thread is mid-callback with --
// has let go, never before.
//
// TrackerSlot uses the pre-C++20 atomic_load/atomic_store free functions
// for std::shared_ptr rather than std::atomic<std::shared_ptr<T>>: the
// class template specialization is not uniformly available across the
// compilers in this project's CI matrix (linux-gcc, linux-clang, macos,
// windows-msvc, windows-clang-cl), while the free functions have been
// part of the standard library since C++11.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace capi_impl
{

// Where create() and destroy() live for a hook type T. The C handle stays
// the address of the T; this registry is what turns that bare address into
// a managed lifetime.
template <typename T>
class TrackerRegistry
{
 public:
  // Builds the object and keeps the master reference alive until destroy()
  // is called with the returned address.
  static T* create(T value)
  {
    auto owned = std::make_shared<T>(std::move(value));
    T* raw = owned.get();
    std::lock_guard<std::mutex> lock(mutex());
    map().emplace(raw, std::move(owned));
    return raw;
  }

  // A strong reference to the object behind `raw`, or an empty shared_ptr
  // if `raw` is null or was already destroyed. Safe to call from any
  // thread, including concurrently with create()/destroy() for other
  // handles of the same type.
  static std::shared_ptr<T> lookup(T* raw)
  {
    if (raw == nullptr)
    {
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex());
    auto it = map().find(raw);
    return it == map().end() ? nullptr : it->second;
  }

  // Drops this registry's reference. The object is actually freed here
  // only if nothing else -- in particular, no TrackerSlot snapshot a
  // dispatch thread took before this call -- still holds a copy.
  static void destroy(T* raw)
  {
    if (raw == nullptr)
    {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex());
    map().erase(raw);
  }

 private:
  static std::mutex& mutex()
  {
    static std::mutex m;
    return m;
  }
  static std::unordered_map<T*, std::shared_ptr<T>>& map()
  {
    static std::unordered_map<T*, std::shared_ptr<T>> m;
    return m;
  }
};

// Atomically-swappable strong reference. What RunnerSignalHandler stores
// per hook; get() is what a dispatch thread calls before touching the
// hook's callbacks, and the returned shared_ptr must be kept alive for the
// duration of that use.
template <typename T>
class TrackerSlot
{
 public:
  void set(std::shared_ptr<T> value) noexcept
  {
    std::atomic_store_explicit(&_current, std::move(value), std::memory_order_release);
  }

  std::shared_ptr<T> get() const noexcept
  {
    return std::atomic_load_explicit(&_current, std::memory_order_acquire);
  }

 private:
  std::shared_ptr<T> _current;
};

}  // namespace capi_impl
