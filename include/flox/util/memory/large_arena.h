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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace flox::memory
{

// EXPERIMENTAL / not yet wired: EventBus and the object pools have no arena
// option or config knob, so nothing in flox currently allocates over a
// LargeArena and a real deployment always reports huge-arena=n/a. The arena
// itself is real (MAP_HUGETLB on Linux); wiring the rings/pools onto it is
// tracked in W25.
//
// Intended backing store for large, long-lived, hot allocations (event-bus
// rings, object pools). 4K pages burn TLB coverage: ~1500 dTLB entries * 4K is
// ~6MB -- a single big ring blows through it and every access pays a page walk.
// 2MB pages cover gigabytes with the same entries and shorten the walk by a
// level.
//
// Three-level fallback, never fatal:
//   1. explicit huge pages: mmap(MAP_HUGETLB), from the vm.nr_hugepages pool
//      reserved at boot. Deterministic: either the pool has room or the call
//      fails right here.
//   2. transparent-huge-page advice: plain mmap + madvise(MADV_HUGEPAGE).
//      Weaker: the kernel may or may not back it with huge pages, and THP
//      compaction can stall -- acceptable outside the colo profile.
//   3. plain pages (always works; the only mode on macOS).
//
// The actual backing is reported, aggregated process-wide, and surfaced in
// the engine startup memory report: running on 4K pages must be a visible
// fact, not a surprise.

class LargeArena
{
 public:
  enum class Backing : uint8_t
  {
    HUGETLB,
    THP_ADVISED,
    PLAIN
  };

  static constexpr size_t kHugePageSize = 2u * 1024u * 1024u;

  explicit LargeArena(size_t bytes, bool preferHuge = true)
  {
    _size = roundUp(bytes, kHugePageSize);

#if defined(__linux__)
    if (preferHuge)
    {
      void* p = ::mmap(nullptr, _size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
      if (p != MAP_FAILED)
      {
        _data = p;
        _backing = Backing::HUGETLB;
      }
    }
    if (!_data)
    {
      void* p = ::mmap(nullptr, _size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p != MAP_FAILED)
      {
        _data = p;
        _backing = Backing::PLAIN;
        if (preferHuge && ::madvise(p, _size, MADV_HUGEPAGE) == 0)
        {
          _backing = Backing::THP_ADVISED;
        }
        prefault();
      }
    }
#elif defined(__APPLE__)
    (void)preferHuge;
    void* p = ::mmap(nullptr, _size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED)
    {
      _data = p;
      _backing = Backing::PLAIN;
      prefault();
    }
#endif

    if (!_data)
    {
      // Last resort on any platform: aligned heap memory.
      _data = ::operator new(_size, std::align_val_t{kHugePageSize});
      _heapBacked = true;
      _backing = Backing::PLAIN;
      prefault();
    }

    bytesFor(_backing).fetch_add(_size, std::memory_order_relaxed);
  }

  ~LargeArena()
  {
    if (!_data)
    {
      return;
    }
    bytesFor(_backing).fetch_sub(_size, std::memory_order_relaxed);
    if (_heapBacked)
    {
      ::operator delete(_data, std::align_val_t{kHugePageSize});
    }
#if defined(__linux__) || defined(__APPLE__)
    else
    {
      ::munmap(_data, _size);
    }
#endif
  }

  LargeArena(const LargeArena&) = delete;
  LargeArena& operator=(const LargeArena&) = delete;

  void* data() noexcept { return _data; }
  size_t size() const noexcept { return _size; }
  Backing backing() const noexcept { return _backing; }

  const char* backingName() const noexcept { return name(_backing); }

  static const char* name(Backing b) noexcept
  {
    switch (b)
    {
      case Backing::HUGETLB:
        return "hugetlb";
      case Backing::THP_ADVISED:
        return "thp-advised";
      case Backing::PLAIN:
        return "plain";
    }
    return "unknown";
  }

  // Touch every page so faults happen here, not on the hot path. HUGETLB with
  // MAP_POPULATE is already resident; calling again is harmless.
  void prefault() noexcept
  {
    auto* p = static_cast<volatile unsigned char*>(_data);
    for (size_t off = 0; off < _size; off += 4096)
    {
      p[off] = 0;
    }
  }

  // Process-wide aggregate for the startup memory report.
  static uint64_t totalBytes(Backing b) { return bytesFor(b).load(std::memory_order_relaxed); }

  static uint64_t totalBytesAll()
  {
    return totalBytes(Backing::HUGETLB) + totalBytes(Backing::THP_ADVISED) +
           totalBytes(Backing::PLAIN);
  }

  // "hugetlb" when everything large sits on explicit huge pages, otherwise
  // the weakest backing present -- the report shows the worst case.
  static const char* aggregateMode()
  {
    if (totalBytesAll() == 0)
    {
      return "n/a";
    }
    if (totalBytes(Backing::PLAIN) > 0)
    {
      return name(Backing::PLAIN);
    }
    if (totalBytes(Backing::THP_ADVISED) > 0)
    {
      return name(Backing::THP_ADVISED);
    }
    return name(Backing::HUGETLB);
  }

 private:
  static size_t roundUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

  static std::atomic<uint64_t>& bytesFor(Backing b)
  {
    static std::atomic<uint64_t> counters[3]{};
    return counters[static_cast<size_t>(b)];
  }

  void* _data{nullptr};
  size_t _size{0};
  Backing _backing{Backing::PLAIN};
  bool _heapBacked{false};
};

// Constructs T inside its own LargeArena, so objects with big inline storage
// (EventBus rings, Pools) land on huge pages without changing their layout:
// the arena owns the whole object, the object keeps its inline arrays.
template <typename T>
class ArenaBacked
{
 public:
  template <typename... Args>
  explicit ArenaBacked(Args&&... args)
      : _arena(sizeof(T)), _obj(::new(_arena.data()) T(std::forward<Args>(args)...))
  {
  }

  ~ArenaBacked() { _obj->~T(); }

  ArenaBacked(const ArenaBacked&) = delete;
  ArenaBacked& operator=(const ArenaBacked&) = delete;

  T* operator->() noexcept { return _obj; }
  T& operator*() noexcept { return *_obj; }
  T* get() noexcept { return _obj; }
  const LargeArena& arena() const noexcept { return _arena; }

 private:
  LargeArena _arena;
  T* _obj;
};

}  // namespace flox::memory
