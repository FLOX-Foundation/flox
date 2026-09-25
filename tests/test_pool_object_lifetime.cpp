/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Pool object lifetime and the acquire/release accounting.
//
// Two things the pool has to do. Every slot it placement-news in its
// constructor it has to unwind in its destructor: a defaulted ~Pool() over
// raw Storage destroyed none of them, so a pooled type holding anything the
// pmr arena does not own -- a std::string, a shared_ptr, a descriptor --
// leaked one per slot, per pool, for the life of the process. And release()
// has to refuse an object that is not currently acquired: an unguarded
// release put one slot index on the freelist twice, so two acquirers were
// handed the same object, and drove inUse()'s `acquired - released`
// subtraction below zero, where size_t turned it into a number near 2^64 --
// the value the exhaustion callback was then given.

#include <gtest/gtest.h>

#include <flox/util/memory/pool.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace flox;

namespace
{

// Counts its own constructions and destructions, and holds a heap resource
// that only a destructor can give back.
struct LifetimeCounters
{
  static inline std::atomic<int> constructed{0};
  static inline std::atomic<int> destructed{0};

  static void reset()
  {
    constructed.store(0);
    destructed.store(0);
  }
};

class CountedEvent : public pool::PoolableBase<CountedEvent>
{
 public:
  explicit CountedEvent(std::pmr::memory_resource*)
      : payload("a resource the arena does not own")
  {
    LifetimeCounters::constructed.fetch_add(1);
  }

  ~CountedEvent() { LifetimeCounters::destructed.fetch_add(1); }

  void clear() { payload.assign("a resource the arena does not own"); }

  std::string payload;
};

constexpr size_t kCapacity = 8;
using CountedPool = pool::Pool<CountedEvent, kCapacity>;

}  // namespace

// The green control: the acquire/release bookkeeping of an ordinary,
// well-behaved cycle is already right, so the failures below are about the
// two specific holes and not about the pool in general.
TEST(PoolObjectLifetime, OrdinaryAcquireAndReleaseAccountsForEverySlot)
{
  LifetimeCounters::reset();
  CountedPool pool;

  EXPECT_EQ(pool.capacity(), kCapacity);
  EXPECT_EQ(pool.inUse(), 0u);

  {
    std::vector<pool::Handle<CountedEvent>> held;
    for (size_t i = 0; i < kCapacity; ++i)
    {
      auto h = pool.acquire();
      ASSERT_TRUE(h.has_value()) << "slot " << i << " should still be free";
      held.push_back(std::move(*h));
    }
    EXPECT_EQ(pool.inUse(), kCapacity);
    EXPECT_FALSE(pool.acquire().has_value()) << "an exhausted pool hands out nothing";
    EXPECT_EQ(pool.exhaustionCount(), 1u);
  }

  EXPECT_EQ(pool.inUse(), 0u);
  EXPECT_EQ(pool.acquireCount(), kCapacity);
  EXPECT_EQ(pool.releaseCount(), kCapacity);
}

TEST(PoolObjectLifetime, EverySlotIsDestroyedExactlyOnceWhenThePoolDies)
{
  LifetimeCounters::reset();

  {
    CountedPool pool;
    ASSERT_EQ(LifetimeCounters::constructed.load(), static_cast<int>(kCapacity))
        << "the pool constructs one object per slot up front";

    // A full round trip, so the slots are in the ordinary post-release state
    // rather than untouched since construction.
    for (size_t i = 0; i < kCapacity; ++i)
    {
      auto h = pool.acquire();
      ASSERT_TRUE(h.has_value());
    }
    EXPECT_EQ(LifetimeCounters::destructed.load(), 0)
        << "returning an object to the pool must not destroy it: the slot is "
           "reused in place";
  }

  EXPECT_EQ(LifetimeCounters::destructed.load(), static_cast<int>(kCapacity))
      << "~Pool() destroyed " << LifetimeCounters::destructed.load() << " of "
      << kCapacity
      << " objects: the slots are raw storage the pool placement-newed into and "
         "never unwinds, so everything a pooled object owns outside the arena "
         "is leaked once per slot";
  EXPECT_EQ(LifetimeCounters::destructed.load(), LifetimeCounters::constructed.load())
      << "constructions and destructions must balance exactly once each";
}

TEST(PoolObjectLifetime, ADoubleReleaseIsRefusedAndInUseNeverGoesNegative)
{
  LifetimeCounters::reset();
  CountedPool pool;

  CountedEvent* raw = nullptr;
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    raw = h->get();
  }  // the handle drops the last reference: one legitimate release

  ASSERT_EQ(pool.acquireCount(), 1u);
  ASSERT_EQ(pool.releaseCount(), 1u);
  ASSERT_EQ(pool.inUse(), 0u);

  // The same object handed back a second time -- a stale Handle copy, a
  // bus slot destroyed twice, a connector that releases what it already
  // published. The pool must refuse it; today it takes it.
  pool.release(raw);

  EXPECT_EQ(pool.releaseCount(), 1u)
      << "the pool accepted a release of an object that was not in use, so the "
         "same slot index now sits on the freelist twice and two acquirers "
         "will be handed the same object";
  EXPECT_EQ(pool.inUse(), 0u)
      << "inUse() reported " << pool.inUse()
      << ": `acquired - released` underflowed in size_t, and that value is what "
         "the exhaustion callback is given";
  EXPECT_LE(pool.inUse(), pool.capacity())
      << "inUse() can never exceed the pool's own capacity";
}

TEST(PoolObjectLifetime, TheFreelistNeverHandsOutOneSlotTwice)
{
  LifetimeCounters::reset();
  CountedPool pool;

  CountedEvent* raw = nullptr;
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    raw = h->get();
  }
  pool.release(raw);  // the bogus second release

  // Whatever the pool did with that release, it must not now believe it owns
  // more free slots than it has.
  std::vector<pool::Handle<CountedEvent>> held;
  std::vector<CountedEvent*> seen;
  for (size_t i = 0; i < kCapacity + 2; ++i)
  {
    auto h = pool.acquire();
    if (!h.has_value())
    {
      break;
    }
    seen.push_back(h->get());
    held.push_back(std::move(*h));
  }

  EXPECT_LE(seen.size(), kCapacity) << "the pool handed out " << seen.size()
                                    << " objects out of " << kCapacity << " slots";

  std::set<CountedEvent*> distinct(seen.begin(), seen.end());
  EXPECT_EQ(distinct.size(), seen.size())
      << seen.size() << " acquisitions returned only " << distinct.size()
      << " distinct objects: the same slot index sits on the freelist more than "
         "once, so two live acquirers are writing the same event";
}

// -------------------------------------------------- the refusal, in detail
//
// Three things the accounting above does not separate. The refusal is a
// compare_exchange on a per-slot claim flag, and a flag read then written
// unconditionally would behave identically for one thread and wrongly for
// two, so the race the exchange exists for has to be driven. indexOf() maps a
// pointer back to a slot, and a mapping that wraps or skips its bounds check
// turns any address in the process into one of this pool's slots. And the
// refusals are counted: invalidReleaseCount() is the machine-readable signal
// a supervisor reads, so it has to move when a release is refused.

TEST(PoolObjectLifetime, TwoThreadsReleasingOneObjectLetExactlyOneThrough)
{
  LifetimeCounters::reset();
  CountedPool pool;

  // Long-lived workers and a per-round gate rather than a thread pair per
  // round: the two releases then start within a few instructions of each
  // other, which is the window the exchange has to close.
  constexpr int kRounds = 2000;
  std::atomic<int> gate{-1};
  std::atomic<int> arrived{0};
  std::atomic<CountedEvent*> target{nullptr};

  auto worker = [&]
  {
    for (int round = 0; round < kRounds; ++round)
    {
      while (gate.load(std::memory_order_acquire) != round)
      {
      }
      pool.release(target.load(std::memory_order_acquire));
      arrived.fetch_add(1, std::memory_order_release);
    }
  };

  // Recorded rather than asserted inside the loop: the workers are still
  // waiting on the gate, and leaving the test body with them running is a
  // terminate rather than a failure. Every round is driven to the end and the
  // first violation is reported after the join.
  struct Violation
  {
    int round{-1};
    size_t inUse{0};
    size_t released{0};
    size_t invalid{0};
    bool acquireFailed{false};
  };
  Violation first;
  bool broken = false;

  std::thread a(worker);
  std::thread b(worker);

  for (int round = 0; round < kRounds; ++round)
  {
    CountedEvent* raw = nullptr;
    {
      auto h = pool.acquire();
      if (h.has_value())
      {
        raw = h->get();
        // One extra reference, so the handle going out of scope does not
        // release the slot: the two explicit releases below are the only
        // ones.
        raw->retain();
      }
      else if (!broken)
      {
        broken = true;
        first = Violation{round, pool.inUse(), pool.releaseCount(),
                          pool.invalidReleaseCount(), true};
      }
    }

    target.store(raw, std::memory_order_release);
    arrived.store(0, std::memory_order_release);
    gate.store(round, std::memory_order_release);
    while (arrived.load(std::memory_order_acquire) != 2)
    {
    }

    if (!broken && (pool.inUse() != 0u ||
                    pool.releaseCount() != static_cast<size_t>(round + 1) ||
                    pool.invalidReleaseCount() != static_cast<size_t>(round + 1)))
    {
      broken = true;
      first = Violation{round, pool.inUse(), pool.releaseCount(),
                        pool.invalidReleaseCount(), false};
    }
  }

  a.join();
  b.join();

  ASSERT_FALSE(broken)
      << "round " << first.round << ": two threads released the same object. "
      << (first.acquireFailed ? "The pool had no slot left to hand out. "
                              : "")
      << "inUse=" << first.inUse << " (expected 0), releaseCount=" << first.released
      << " (expected " << (first.round + 1) << ": exactly one of the two may be taken), "
      << "invalidReleaseCount=" << first.invalid << " (expected " << (first.round + 1)
      << ": the loser of the race must be counted, not ignored)";

  // The slot survived every round: still exactly one object, still handed out
  // once at a time.
  EXPECT_EQ(pool.inUse(), 0u);
  std::set<CountedEvent*> distinct;
  std::vector<pool::Handle<CountedEvent>> held;
  for (size_t i = 0; i < kCapacity; ++i)
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value()) << "slot " << i << " was lost to the race";
    distinct.insert(h->get());
    held.push_back(std::move(*h));
  }
  EXPECT_EQ(distinct.size(), kCapacity);
  EXPECT_FALSE(pool.acquire().has_value());
}

TEST(PoolObjectLifetime, APointerThisPoolDidNotHandOutIsRefusedAndCounted)
{
  LifetimeCounters::reset();
  CountedPool pool;
  CountedPool sibling;  // same T, same Capacity, different slots

  // Every slot claimed, so a pointer that is wrongly mapped onto one of them
  // finds a claim to take rather than an already-free slot that would be
  // refused for the wrong reason.
  std::vector<pool::Handle<CountedEvent>> held;
  for (size_t i = 0; i < kCapacity; ++i)
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    held.push_back(std::move(*h));
  }
  ASSERT_EQ(pool.inUse(), kCapacity);
  ASSERT_EQ(pool.releaseCount(), 0u);

  auto siblingHandle = sibling.acquire();
  ASSERT_TRUE(siblingHandle.has_value());
  CountedEvent* foreign = siblingHandle->get();

  pool.release(foreign);

  EXPECT_EQ(pool.invalidReleaseCount(), 1u)
      << "a pointer from another pool of the same type was not refused: "
         "indexOf() mapped an address this pool never handed out onto one of "
         "its own slots";
  EXPECT_EQ(pool.releaseCount(), 0u) << "no release of this pool's own may have been taken";
  EXPECT_EQ(pool.inUse(), kCapacity) << "every slot is still held by a live handle";
  auto stolen = pool.acquire();
  EXPECT_FALSE(stolen.has_value())
      << "a slot came free out of a foreign pointer: it is about to be handed "
         "to a second acquirer while its first one is still writing it";
  if (stolen.has_value())
  {
    // The pool is already corrupt at this point: this object is also owned by
    // a handle in `held`, and the acquire has just reset its reference count
    // under that owner. One extra reference so the two owners do not drive
    // the count below zero at teardown, which aborts the process and takes
    // the rest of this test's report with it.
    stolen->get()->retain();
  }

  // A plain automatic object of the same type -- an address nowhere near the
  // slot array.
  CountedEvent local(nullptr);
  pool.release(&local);

  EXPECT_EQ(pool.invalidReleaseCount(), 2u) << "a stack address was not refused";
  EXPECT_EQ(pool.releaseCount(), 0u);
  EXPECT_EQ(pool.inUse(), kCapacity);

  pool.release(nullptr);
  EXPECT_EQ(pool.invalidReleaseCount(), 3u);

  // The sibling is untouched by any of it.
  EXPECT_EQ(sibling.inUse(), 1u);
  EXPECT_EQ(sibling.releaseCount(), 0u);
  EXPECT_EQ(sibling.invalidReleaseCount(), 0u);
}

TEST(PoolObjectLifetime, EveryRefusedReleaseIsCounted)
{
  LifetimeCounters::reset();
  CountedPool pool;
  CountedPool sibling;

  CountedEvent* raw = nullptr;
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    raw = h->get();
  }  // the handle's own release: the one legitimate one

  ASSERT_EQ(pool.releaseCount(), 1u);
  ASSERT_EQ(pool.invalidReleaseCount(), 0u) << "nothing has been refused yet";

  pool.release(raw);  // the same object handed back a second time
  EXPECT_EQ(pool.invalidReleaseCount(), 1u)
      << "the double release was refused but not counted, so a supervisor "
         "reading invalidReleaseCount() cannot see a caller releasing twice";

  auto siblingHandle = sibling.acquire();
  ASSERT_TRUE(siblingHandle.has_value());
  pool.release(siblingHandle->get());  // a pointer from another pool
  EXPECT_EQ(pool.invalidReleaseCount(), 2u)
      << "the foreign release was refused but not counted";

  EXPECT_EQ(pool.releaseCount(), 1u) << "neither refusal may count as a release";
  EXPECT_EQ(pool.inUse(), 0u);
}

namespace
{

// A pooled type whose clear() touches nothing but an int. The alignment test
// below hands the pool a pointer that is inside the slot array but not at the
// start of a slot, and a pool that wrongly accepts it calls clear() through
// that pointer: with a std::string in the object that is a free() of whatever
// the misaligned bytes happen to look like, which ends the process before the
// test can say what went wrong. An int write there is survivable, so the
// failure is an assertion rather than a crash.
struct PlainEvent : public pool::PoolableBase<PlainEvent>
{
  explicit PlainEvent(std::pmr::memory_resource*) {}

  void clear() { value = 0; }

  int value{0};
};

using PlainPool = pool::Pool<PlainEvent, kCapacity>;

}  // namespace

// A pointer into this pool's storage that is not the start of a slot is not
// one of its objects. indexOf() answers a release with the slot it belongs
// to, and a division alone cannot tell "slot 3" from "slot 3 plus one byte"
// -- so the remainder is checked as well. Without that check an address
// anywhere inside a claimed slot releases that slot: the object's owner is
// still holding it, and the next acquirer gets the same one.
TEST(PoolObjectLifetime, AMisalignedPointerIntoTheSlotArrayIsRefused)
{
  PlainPool pool;

  std::vector<pool::Handle<PlainEvent>> held;
  for (size_t i = 0; i < kCapacity; ++i)
  {
    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());
    held.push_back(std::move(*h));
  }
  ASSERT_EQ(pool.inUse(), kCapacity);
  ASSERT_EQ(pool.releaseCount(), 0u);
  ASSERT_EQ(pool.invalidReleaseCount(), 0u);

  // Both addresses are inside slots this pool has handed out and is holding,
  // which is what makes a missing alignment check decide they are live
  // objects rather than refuse them.
  auto* const slotOne = reinterpret_cast<std::byte*>(held[1].get());
  auto* const slotTwo = reinterpret_cast<std::byte*>(held[2].get());

  pool.release(reinterpret_cast<PlainEvent*>(slotOne + 1));
  EXPECT_EQ(pool.invalidReleaseCount(), 1u)
      << "a pointer one byte into slot 1 was taken as slot 1 itself";
  EXPECT_EQ(pool.releaseCount(), 0u);
  EXPECT_EQ(pool.inUse(), kCapacity);

  pool.release(reinterpret_cast<PlainEvent*>(slotTwo + sizeof(PlainEvent) / 2));
  EXPECT_EQ(pool.invalidReleaseCount(), 2u)
      << "a pointer half a slot into slot 2 was taken as slot 2 itself";
  EXPECT_EQ(pool.releaseCount(), 0u)
      << "no slot may be released by an address that is not one of the "
         "objects this pool handed out";
  EXPECT_EQ(pool.inUse(), kCapacity) << "every slot is still held by a live handle";

  auto stolen = pool.acquire();
  EXPECT_FALSE(stolen.has_value())
      << "a slot came free out of a misaligned pointer: it is about to be "
         "handed to a second acquirer while its first one still holds it";
  if (stolen.has_value())
  {
    // Already corrupt: this object has an owner in `held` whose reference
    // count the acquire has just reset. One extra reference so the two owners
    // do not drive it below zero at teardown, which aborts the process and
    // takes the rest of the report with it.
    stolen->get()->retain();
  }

  // The slots are handed out back to front, so the array's ends are the
  // lowest and highest addresses seen rather than the first and last handle.
  std::byte* lowest = reinterpret_cast<std::byte*>(held[0].get());
  std::byte* highest = lowest;
  for (const auto& h : held)
  {
    auto* const addr = reinterpret_cast<std::byte*>(h.get());
    lowest = std::min(lowest, addr);
    highest = std::max(highest, addr);
  }

  // One slot past the end of the array, and one slot below its start:
  // correctly aligned, and neither of them a slot of this pool.
  pool.release(reinterpret_cast<PlainEvent*>(highest + sizeof(PlainEvent)));
  pool.release(reinterpret_cast<PlainEvent*>(lowest - sizeof(PlainEvent)));
  EXPECT_EQ(pool.invalidReleaseCount(), 4u) << "an address outside the slot array is not a slot";
  EXPECT_EQ(pool.releaseCount(), 0u);
  EXPECT_EQ(pool.inUse(), kCapacity);
}
