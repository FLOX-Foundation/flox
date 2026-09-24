/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// stop() against a live publisher.
//
// A publisher reads _running once, at the top of publish(), and everything
// after that read -- claiming a sequence, destroying what the slot held,
// constructing the event into it, stamping the slot -- runs with no further
// agreement with stop(). stop() meanwhile destroys every constructed slot and
// puts the sequence line back where a fresh bus starts. The two overlap.
//
// The overlap is pinned three ways here: deterministically, by making the
// event's copy constructor slow enough that the window is milliseconds wide;
// by counting, because an event that was accepted and then thrown away by the
// teardown is a silent loss; and as a stress loop whose assertion is
// ThreadSanitizer.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

// The copy constructor is the bus's construct-into-the-slot step, so a probe
// inside it observes exactly the interval stop() must not overlap.
struct SlowCopyEvent
{
  using Listener = struct ISlowCopyListener
  {
    virtual ~ISlowCopyListener() = default;
    virtual void onEvent(const SlowCopyEvent& e) = 0;
  };

  struct Probe
  {
    std::atomic<bool> inCopy{false};
    std::atomic<bool> copyDone{false};
    std::chrono::milliseconds delay{0};
  };

  // Armed by the test around the one publish it wants to catch mid-flight.
  static Probe* probe;

  int value{0};
  uint64_t tickSequence{0};

  SlowCopyEvent() = default;

  SlowCopyEvent(const SlowCopyEvent& other)
      : value(other.value), tickSequence(other.tickSequence)
  {
    Probe* p = probe;
    if (p == nullptr || p->delay.count() == 0)
    {
      return;
    }
    p->inCopy.store(true, std::memory_order_release);
    std::this_thread::sleep_for(p->delay);
    p->copyDone.store(true, std::memory_order_release);
    p->inCopy.store(false, std::memory_order_release);
  }

  SlowCopyEvent& operator=(const SlowCopyEvent& other)
  {
    value = other.value;
    tickSequence = other.tickSequence;
    return *this;
  }
};

SlowCopyEvent::Probe* SlowCopyEvent::probe = nullptr;

template <>
struct EventDispatcher<SlowCopyEvent>
{
  static void dispatch(const SlowCopyEvent& event, SlowCopyEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

// The bus writes the sequence into the event AFTER it has stamped the slot as
// constructed, and that write is the one place where a live publisher and
// stop()'s teardown touch the same bytes with nothing ordering them. In the
// real thing the window is two instructions wide; a proxy whose assignment
// takes its time makes it microseconds, which is what gives a race detector
// something to catch.
struct Stamp
{
  static std::atomic<int> delayUs;

  uint64_t value{0};

  Stamp& operator=(uint64_t v)
  {
    const int delay = delayUs.load(std::memory_order_relaxed);
    if (delay != 0)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
    value = v;
    return *this;
  }
};

std::atomic<int> Stamp::delayUs{0};

// A destructor that touches the object, the way an event holding a pool
// handle or a refcount does. Without one the teardown's ~Event() compiles to
// nothing and its overlap with a publisher still inside the ring is invisible
// to a race detector.
struct LifecycleEvent
{
  using Listener = struct ILifecycleListener
  {
    virtual ~ILifecycleListener() = default;
    virtual void onEvent(const LifecycleEvent& e) = 0;
  };

  // Microseconds the copy into the ring slot costs. Zero by default; a test
  // that has to catch a publisher mid-construction widens the window with it,
  // because the real one is a few dozen nanoseconds wide.
  static std::atomic<int> copyDelayUs;

  int64_t payload[8]{};
  int value{0};
  Stamp tickSequence{};

  ~LifecycleEvent()
  {
    tickSequence.value = 0;
    payload[0] = -1;
  }

  LifecycleEvent() = default;

  LifecycleEvent(const LifecycleEvent& other) : value(other.value)
  {
    tickSequence.value = other.tickSequence.value;
    for (size_t i = 0; i < 8; ++i)
    {
      payload[i] = other.payload[i];
    }
    const int delay = copyDelayUs.load(std::memory_order_relaxed);
    if (delay != 0)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
  }

  LifecycleEvent& operator=(const LifecycleEvent&) = default;
};

std::atomic<int> LifecycleEvent::copyDelayUs{0};

template <>
struct EventDispatcher<LifecycleEvent>
{
  static void dispatch(const LifecycleEvent& event, LifecycleEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;

using SlowBus = EventBus<SlowCopyEvent, 64, 4>;
using LifecycleBus = EventBus<LifecycleEvent, 64, 4>;

class SlowCounting : public SlowCopyEvent::Listener
{
 public:
  void onEvent(const SlowCopyEvent& e) override
  {
    last.store(e.value, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
  }
  std::atomic<int> count{0};
  std::atomic<int> last{0};
};

class LifecycleCounting : public LifecycleEvent::Listener
{
 public:
  void onEvent(const LifecycleEvent& e) override
  {
    last.store(e.value, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
  }
  std::atomic<uint64_t> count{0};
  std::atomic<int> last{0};
};

// The deterministic half: the publisher is parked inside the placement-new for
// 300 ms and stop() lands in the middle of it. stop() returning is the moment
// the ring may be torn down and the bus destroyed, so the question this asks
// is whether the publisher had left the ring by then.
TEST(EventBusLifecycle, StopWaitsForAPublisherAlreadyInsideTheRing)
{
  SlowBus bus;
  SlowCounting listener;
  ASSERT_TRUE(bus.subscribe(&listener));
  bus.start();

  SlowCopyEvent::Probe probe;
  probe.delay = std::chrono::milliseconds(300);
  SlowCopyEvent::probe = &probe;

  std::thread publisher(
      [&bus]
      {
        SlowCopyEvent ev;
        ev.value = 7;
        bus.publish(ev);
      });

  // Raised from inside the copy constructor: after the sequence was claimed
  // and while the slot is being written.
  while (!probe.inCopy.load(std::memory_order_acquire))
  {
    std::this_thread::yield();
  }

  bus.stop();
  const bool leftTheRing = probe.copyDone.load(std::memory_order_acquire);

  publisher.join();
  SlowCopyEvent::probe = nullptr;

  EXPECT_TRUE(leftTheRing)
      << "stop() returned while a publisher was still constructing into a slot";
}

// An event the bus accepted (publish returned a sequence) and then dropped on
// the way down is a silent loss: the publisher has no way to learn about it.
// Either the publish is refused -- the bus says no, the caller knows -- or the
// event is delivered. Counted over rounds that stop at a different instant
// each time.
TEST(EventBusLifecycle, EveryPublishAcrossAStopIsEitherDeliveredOrRefused)
{
  for (int round = 0; round < 8; ++round)
  {
    LifecycleBus bus;
    LifecycleCounting listener;
    ASSERT_TRUE(bus.subscribe(&listener, /*required=*/true));
    bus.enableDrainOnStop();
    bus.start();
    LifecycleEvent::copyDelayUs.store(50, std::memory_order_relaxed);

    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<bool> go{false};

    std::thread publisher(
        [&bus, &accepted, &refused, &go]
        {
          while (!go.load(std::memory_order_acquire))
          {
            std::this_thread::yield();
          }
          LifecycleEvent ev;
          for (int i = 0; i < 2000000; ++i)
          {
            ev.value = i;
            if (bus.publish(ev) >= 0)
            {
              accepted.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
              refused.fetch_add(1, std::memory_order_relaxed);
              break;  // the bus said no; nothing after this can be accepted
            }
          }
        });

    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(1 + round));
    bus.stop();
    publisher.join();
    LifecycleEvent::copyDelayUs.store(0, std::memory_order_relaxed);

    const uint64_t delivered = listener.count.load(std::memory_order_relaxed);
    const uint64_t taken = accepted.load(std::memory_order_relaxed);
    ASSERT_GT(taken, 0u) << "round " << round << ": the publisher never got going";
    ASSERT_GT(refused.load(std::memory_order_relaxed), 0u)
        << "round " << round << ": the publisher finished before stop(); nothing was raced";
    EXPECT_EQ(delivered, taken)
        << "round " << round << ": " << (taken - delivered)
        << " events were accepted by publish() and never delivered";
  }
}

// The stress half: publishers in a loop against a stop. In a plain build this
// only has to survive; under ThreadSanitizer it is the reproduction, because
// the construct/destroy overlap on the slot bytes is a race TSan names. The
// restart at the end is the outcome assertion that accompanies it: stop()
// returning has to mean the ring is quiescent and usable again.
TEST(EventBusLifecycle, PublishInALoopAgainstStopIsRaceFree)
{
  for (int round = 0; round < 24; ++round)
  {
    LifecycleBus bus;
    LifecycleCounting listener;
    ASSERT_TRUE(bus.subscribe(&listener, /*required=*/false));
    bus.start();

    Stamp::delayUs.store(5, std::memory_order_relaxed);

    std::atomic<bool> go{false};
    std::vector<std::thread> publishers;
    for (int t = 0; t < 3; ++t)
    {
      publishers.emplace_back(
          [&bus, &go]
          {
            while (!go.load(std::memory_order_acquire))
            {
              std::this_thread::yield();
            }
            LifecycleEvent ev;
            for (int i = 0; i < 100000; ++i)
            {
              ev.value = i;
              if (bus.publish(ev) < 0)
              {
                break;  // the bus stopped underneath; that is the point
              }
            }
          });
    }

    go.store(true, std::memory_order_release);
    // A different stop instant every round, so the teardown lands somewhere
    // else in the publishers' loops each time.
    std::this_thread::sleep_for(std::chrono::microseconds(700 + 137 * round));
    bus.stop();

    for (auto& p : publishers)
    {
      p.join();
    }
    Stamp::delayUs.store(0, std::memory_order_relaxed);

    bus.start();
    LifecycleEvent ev;
    ev.value = 42;
    EXPECT_GE(bus.publish(ev), 0) << "round " << round << ": the bus did not come back up";
    bus.stop();
  }
}

// Control: the same shapes with no overlap at all. A stop that follows the
// last publish delivers everything and loses nothing, today and after.
TEST(EventBusLifecycle, StopAfterTheLastPublishDeliversEverything)
{
  LifecycleBus bus;
  LifecycleCounting listener;
  ASSERT_TRUE(bus.subscribe(&listener, /*required=*/true));
  bus.enableDrainOnStop();
  bus.start();

  LifecycleEvent ev;
  for (int i = 0; i < 1000; ++i)
  {
    ev.value = i;
    ASSERT_GE(bus.publish(ev), 0);
  }
  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(std::memory_order_relaxed), 1000u);
  EXPECT_EQ(listener.last.load(std::memory_order_relaxed), 999);
}

}  // namespace
