/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The market-data hot path must not allocate in steady state. Nothing used to
// check that: the property held by discipline, and the pool's arena silently
// falls back to the heap once its inline buffer is exhausted, so a book deeper
// than the pool was sized for allocated mid-session with no signal.
//
// This binary replaces the global operator new with a counter, which is why it
// is a separate executable rather than a case in a shared test binary.

#include "flox/book/events/book_update_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/util/eventing/event_bus.h"
#include "flox/util/memory/pool.h"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

// Replacing the global operator new is fundamentally incompatible with the
// sanitizers: they interpose new/delete themselves, so an allocation served
// by the sanitizer's operator new and released through this file's delete
// (which goes to free) is reported as alloc-dealloc-mismatch -- on gtest's
// own memory, before any flox code runs. Under a sanitizer build the counter
// is left alone and the cases skip; the guard still runs in every normal
// build, which is where it does its job.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FLOX_TEST_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define FLOX_TEST_UNDER_SANITIZER 1
#endif
#endif

#ifndef FLOX_TEST_UNDER_SANITIZER
#define FLOX_TEST_UNDER_SANITIZER 0
#endif

#if FLOX_TEST_UNDER_SANITIZER
#define SKIP_UNDER_SANITIZER()                                         \
  GTEST_SKIP() << "allocation counting replaces global operator new, " \
                  "which the sanitizers interpose; covered by the "    \
                  "non-sanitizer builds"
#else
#define SKIP_UNDER_SANITIZER() ((void)0)
#endif

namespace
{

std::atomic<long> g_allocations{0};
std::atomic<bool> g_counting{false};

struct CountingScope
{
  CountingScope()
  {
    g_allocations.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
  }
  ~CountingScope() { g_counting.store(false, std::memory_order_relaxed); }
  long count() const { return g_allocations.load(std::memory_order_relaxed); }
};

using namespace flox;

// The bus slot holds the Handle until that slot is overwritten, so a pool
// smaller than the ring drains permanently and nothing circulates. Keep the
// ring small and the pool larger than it, which is the steady state a live
// deployment is in once the ring has wrapped.
constexpr size_t kRingCapacity = 64;
constexpr size_t kPoolCapacity = 255;
constexpr int kMaxDepth = 512;

struct DepthSink : public IMarketDataSubscriber
{
  SubscriberId id() const override { return 1; }
  void onBookUpdate(const BookUpdateEvent& ev) override { seen += ev.update.bids.size(); }
  size_t seen = 0;
};

using BookPool = pool::Pool<BookUpdateEvent, kPoolCapacity>;
using TestBus = EventBus<pool::Handle<BookUpdateEvent>, kRingCapacity>;

void publish(BookPool& pool, TestBus& bus, int events, int depth)
{
  for (int i = 0; i < events; ++i)
  {
    auto handle = pool.acquire();
    if (!handle)
    {
      continue;
    }
    auto& ev = **handle;
    ev.update.symbol = 1;
    ev.update.type = BookUpdateType::DELTA;
    for (int level = 0; level < depth; ++level)
    {
      ev.update.bids.emplace_back(Price::fromDouble(100.0 - level), Quantity::fromDouble(1.0));
      ev.update.asks.emplace_back(Price::fromDouble(100.0 + level), Quantity::fromDouble(1.0));
    }
    bus.publish(std::move(*handle));
  }
}

}  // namespace

#if !FLOX_TEST_UNDER_SANITIZER

void* operator new(size_t bytes)
{
  if (g_counting.load(std::memory_order_relaxed))
  {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  void* p = std::malloc(bytes);
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

namespace
{

// MSVC's CRT has no std::aligned_alloc, and memory from _aligned_malloc must
// go back through _aligned_free rather than free().
void* alignedAlloc(size_t bytes, size_t align)
{
#if defined(_MSC_VER)
  return _aligned_malloc(bytes, align);
#else
  const size_t rounded = ((bytes + align - 1) / align) * align;
  return std::aligned_alloc(align, rounded);
#endif
}

void alignedFree(void* p) noexcept
{
#if defined(_MSC_VER)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

}  // namespace

// pmr resources allocate through the aligned overload; counting only the
// plain one is how this guard silently measured nothing at first.
void* operator new(size_t bytes, std::align_val_t align)
{
  if (g_counting.load(std::memory_order_relaxed))
  {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  void* p = alignedAlloc(bytes, static_cast<size_t>(align));
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { alignedFree(p); }

#endif  // !FLOX_TEST_UNDER_SANITIZER

TEST(HotPathAllocation, SteadyStatePublishDoesNotAllocate)
{
  SKIP_UNDER_SANITIZER();
  auto pool = std::make_unique<BookPool>();
  TestBus bus;
  DepthSink sink;
  bus.subscribe(&sink);
  bus.start();

  publish(*pool, bus, 2000, 32);  // warm the arena and the vectors

  {
    CountingScope counting;
    publish(*pool, bus, 20000, 32);
    EXPECT_EQ(counting.count(), 0) << "market-data publish allocated in steady state";
  }

  // Guard the guard: if the pool drains, publish() skips every event and the
  // measurement above is of an empty loop rather than of the hot path. That
  // is exactly how this test read "zero allocations" while testing nothing.
  EXPECT_EQ(pool->exhaustionCount(), 0u) << "pool drained -- the test measured an idle loop";
  EXPECT_GT(pool->releaseCount(), 20000u) << "objects never circulated back to the pool";

  bus.stop();
}

// The regression that motivated this file: a book that deepens mid-session
// grows the pooled vectors, which the arena serves from the heap once its
// inline buffer runs out -- an allocation in the middle of trading.
TEST(HotPathAllocation, PrewarmAbsorbsALaterDepthIncrease)
{
  SKIP_UNDER_SANITIZER();
  auto pool = std::make_unique<BookPool>();
  TestBus bus;
  DepthSink sink;
  bus.subscribe(&sink);
  bus.start();

  pool->prewarm(
      [](BookUpdateEvent& ev)
      {
        ev.update.bids.reserve(kMaxDepth);
        ev.update.asks.reserve(kMaxDepth);
      });

  publish(*pool, bus, 500, 16);

  {
    CountingScope counting;
    publish(*pool, bus, 2000, kMaxDepth);
    EXPECT_EQ(counting.count(), 0) << "depth increase allocated despite prewarm";
  }

  bus.stop();
}

// Escaping to the heap must be observable rather than silent, so an
// undersized pool can be caught in the startup memory report.
TEST(HotPathAllocation, ArenaHeapFallbackIsCounted)
{
  // No global-new interposition here -- this reads the pool's own counter,
  // so it runs under the sanitizers too.
  auto pool = std::make_unique<BookPool>();
  const uint64_t before = pool->upstreamAllocations();

  pool->prewarm(
      [](BookUpdateEvent& ev)
      {
        ev.update.bids.reserve(8192);
        ev.update.asks.reserve(8192);
      });

  EXPECT_GT(pool->upstreamAllocations(), before)
      << "arena fell back to the heap without counting it";
  EXPECT_GT(pool->upstreamBytes(), 0u);
  EXPECT_GE(memory::CountingResource::totalAllocations(), pool->upstreamAllocations());
}
