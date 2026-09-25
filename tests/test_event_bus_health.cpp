/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

struct HealthTestEvent
{
  using Listener = struct IHealthTestListener
  {
    virtual ~IHealthTestListener() = default;
    virtual void onEvent(const HealthTestEvent& e) = 0;
  };

  int value{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<HealthTestEvent>
{
  static void dispatch(const HealthTestEvent& event, HealthTestEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;

constexpr size_t kCapacity = 64;
using SmallBus = EventBus<HealthTestEvent, kCapacity, 4>;

class Counting : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent& e) override
  {
    ++count;
    lastValue = e.value;
  }
  std::atomic<int> count{0};
  std::atomic<int> lastValue{0};
};

class Blocking : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent&) override
  {
    ++entered;
    while (blocked.load())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++count;
  }
  std::atomic<int> entered{0};
  std::atomic<bool> blocked{true};
  std::atomic<int> count{0};
};

class ThrowingOnFirst : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent&) override
  {
    throw std::runtime_error("handler failure");
  }
};

struct CallbackRecord
{
  std::atomic<int> stalledEvents{0};
  std::atomic<int> deadEvents{0};
  std::atomic<int> healthyEvents{0};
};

void recordCallback(uint32_t, SmallBus::ConsumerHealth state, void* user)
{
  auto* rec = static_cast<CallbackRecord*>(user);
  switch (state)
  {
    case SmallBus::ConsumerHealth::STALLED:
      ++rec->stalledEvents;
      break;
    case SmallBus::ConsumerHealth::DEAD:
      ++rec->deadEvents;
      break;
    case SmallBus::ConsumerHealth::HEALTHY:
      ++rec->healthyEvents;
      break;
  }
}

class SlowEach : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent&) override
  {
    // Slow but always returns to the loop top -- drop-behind fires there.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++count;
  }
  std::atomic<int> count{0};
};

TEST(EventBusHealth, DropBehindOptionalDoesNotStallPublisher)
{
  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.dropBehindOptional = true;
  cfg.dropBehindSlack = kCapacity / 2;
  bus.setHealthConfig(cfg);

  Counting fast;
  SlowEach slow;
  bus.subscribe(&fast, /*required=*/true);
  bus.subscribe(&slow, /*required=*/false);
  bus.start();

  // Flood far beyond capacity while the optional consumer is stuck in its
  // first event. Without drop-behind the publisher would freeze at the
  // reclaim fence once the ring wraps.
  constexpr int kEvents = int(kCapacity) * 8;
  for (int i = 0; i < kEvents; ++i)
  {
    HealthTestEvent ev;
    ev.value = i;
    ASSERT_GE(bus.publish(ev), 0) << "publisher stalled at event " << i;
  }

  // Fast required consumer sees everything.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (fast.count.load() < kEvents && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(fast.count.load(), kEvents);

  bus.stop();

  const auto stats = bus.stats();
  EXPECT_GT(stats.droppedBehind, 0u);
  EXPECT_EQ(stats.published, uint64_t(kEvents));
}

TEST(EventBusHealth, CheckHealthReportsStallAndRecovery)
{
  SmallBus bus;
  CallbackRecord rec;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(20);
  cfg.callback = &recordCallback;
  cfg.callbackUser = &rec;
  bus.setHealthConfig(cfg);

  Blocking blocking;
  bus.subscribe(&blocking, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 1;
  bus.publish(ev);
  ev.value = 2;
  bus.publish(ev);  // pending work behind the blocked consumer

  // Wait until the consumer is inside the handler, then let the stall age.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (blocking.entered.load() == 0 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_GT(blocking.entered.load(), 0);

  bus.checkHealth();  // baseline sweep records lastSeen
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const auto sweep = bus.checkHealth();
  EXPECT_EQ(sweep.stalled, 1u);
  EXPECT_EQ(rec.stalledEvents.load(), 1);

  // Recovery: unblock, wait for progress, next sweep reports healthy.
  blocking.blocked = false;
  while (blocking.count.load() < 2 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto sweep2 = bus.checkHealth();
  EXPECT_EQ(sweep2.stalled, 0u);
  EXPECT_EQ(rec.healthyEvents.load(), 1);

  bus.stop();
}

TEST(EventBusHealth, ThrowingHandlerIsDetectedAsDead)
{
  SmallBus bus;
  CallbackRecord rec;
  SmallBus::HealthConfig cfg;
  cfg.callback = &recordCallback;
  cfg.callbackUser = &rec;
  bus.setHealthConfig(cfg);

  ThrowingOnFirst throwing;
  Counting healthy;
  bus.subscribe(&throwing, /*required=*/false);
  bus.subscribe(&healthy, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 7;
  bus.publish(ev);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  SmallBus::HealthSweep sweep{};
  while (std::chrono::steady_clock::now() < deadline)
  {
    sweep = bus.checkHealth();
    if (sweep.dead > 0)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(sweep.dead, 1u);
  EXPECT_EQ(rec.deadEvents.load(), 1);

  // The rest of the bus keeps working.
  ev.value = 8;
  bus.publish(ev);
  while (healthy.count.load() < 2 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(healthy.count.load(), 2);

  bus.stop();
}

TEST(EventBusHealth, DeadRequiredConsumerStopsBusUnderStopPolicy)
{
  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.deadPolicy = SmallBus::DeadConsumerPolicy::STOP_BUS;
  bus.setHealthConfig(cfg);

  ThrowingOnFirst throwing;
  bus.subscribe(&throwing, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 1;
  bus.publish(ev);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool stopped = false;
  while (std::chrono::steady_clock::now() < deadline)
  {
    bus.checkHealth();
    HealthTestEvent probe;
    probe.value = 2;
    if (bus.publish(probe) < 0)
    {
      stopped = true;  // publish on a stopped bus returns -1
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(stopped);
}

TEST(EventBusHealth, MonitorThreadSweepsAutomatically)
{
  SmallBus bus;
  CallbackRecord rec;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(10);
  cfg.enableMonitorThread = true;
  cfg.callback = &recordCallback;
  cfg.callbackUser = &rec;
  bus.setHealthConfig(cfg);

  Blocking blocking;
  bus.subscribe(&blocking, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 1;
  bus.publish(ev);
  ev.value = 2;
  bus.publish(ev);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (rec.stalledEvents.load() == 0 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GT(rec.stalledEvents.load(), 0);

  blocking.blocked = false;
  bus.stop();
}

// Regression: STOP_BUS tripped from the monitor thread used to call stop(),
// which reset (joined) the monitor thread from within its own body -> self-join
// -> std::terminate. This must complete without crashing.
TEST(EventBusHealth, StopBusFromMonitorThreadDoesNotSelfJoin)
{
  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.deadPolicy = SmallBus::DeadConsumerPolicy::STOP_BUS;
  cfg.stallThreshold = std::chrono::milliseconds(10);
  cfg.enableMonitorThread = true;
  bus.setHealthConfig(cfg);

  ThrowingOnFirst throwing;
  bus.subscribe(&throwing, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 1;
  bus.publish(ev);

  // The monitor thread detects the dead consumer and stops the bus on its own.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool stopped = false;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (bus.publish(ev) < 0)
    {
      stopped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(stopped);

  // Level-triggered query works after the bus stopped itself.
  EXPECT_EQ(bus.consumerHealth(0), SmallBus::ConsumerHealth::DEAD);
  EXPECT_EQ(bus.healthSnapshot().dead, 1u);

  bus.stop();  // idempotent; must not double-join or crash
}

// ---------------------------------------------------------------------------
// The monitor thread's period.
//
// It is stallThreshold / 2 in integer milliseconds, so any threshold below
// 2 ms truncates to zero and the monitor calls checkHealth() -- an
// O(consumers) scan over atomics -- as fast as the core will let it. 1 ms is
// where the arithmetic first collapses, and a plausible setting for a bus on
// which a millisecond of stall matters.

// Process CPU time so far, in milliseconds. A thread that sleeps out a window
// costs none of it; a thread that spins through it costs the whole window.
double processCpuMillis()
{
  return 1000.0 * static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// Nothing is published in these two, and the consumer parks rather than
// spinning, so the only thread that can burn CPU is the monitor.
constexpr auto kCpuWindow = std::chrono::milliseconds(300);
constexpr double kIdleBudgetMillis = 60.0;

TEST(EventBusMonitor, AMillisecondStallThresholdDoesNotSpinTheMonitorThread)
{
  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(1);
  cfg.enableMonitorThread = true;
  bus.setHealthConfig(cfg);

  Counting listener;
  ASSERT_TRUE(bus.subscribe(&listener, /*required=*/true, SmallBus::WaitMode::PARKED));
  bus.start();

  const double before = processCpuMillis();
  std::this_thread::sleep_for(kCpuWindow);
  const double spent = processCpuMillis() - before;
  bus.stop();

  EXPECT_LT(spent, kIdleBudgetMillis)
      << "an idle bus burned " << spent << " ms of CPU over a " << kCpuWindow.count()
      << " ms window: the monitor period truncated to a zero sleep";
}

// Control: the same bus with the default threshold, where the halving does not
// truncate. Idle costs nothing, today and after.
TEST(EventBusMonitor, ADefaultStallThresholdLeavesTheMonitorThreadIdle)
{
  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(100);
  cfg.enableMonitorThread = true;
  bus.setHealthConfig(cfg);

  Counting listener;
  ASSERT_TRUE(bus.subscribe(&listener, /*required=*/true, SmallBus::WaitMode::PARKED));
  bus.start();

  const double before = processCpuMillis();
  std::this_thread::sleep_for(kCpuWindow);
  const double spent = processCpuMillis() - before;
  bus.stop();

  EXPECT_LT(spent, kIdleBudgetMillis)
      << "an idle bus burned " << spent << " ms of CPU over a " << kCpuWindow.count()
      << " ms window";
}

// The same floor stated directly, so the fix is a rule rather than a number
// that happens to keep one measurement under a budget.
// needs: static std::chrono::milliseconds monitorPeriod(std::chrono::milliseconds stallThreshold)
TEST(EventBusMonitor, TheMonitorPeriodIsHalfTheThresholdWithAFloor)
{
  const auto check = [](auto& bus)
  {
    using Bus = std::decay_t<decltype(bus)>;
    using Ms = std::chrono::milliseconds;
    if constexpr (requires { Bus::monitorPeriod(Ms(1)); })
    {
      EXPECT_GE(Bus::monitorPeriod(Ms(0)), Ms(1));
      EXPECT_GE(Bus::monitorPeriod(Ms(1)), Ms(1));
      EXPECT_GE(Bus::monitorPeriod(Ms(2)), Ms(1));
      EXPECT_GE(Bus::monitorPeriod(Ms(3)), Ms(1));
      EXPECT_EQ(Bus::monitorPeriod(Ms(100)), Ms(50));
    }
    else
    {
      FAIL() << "needs: static std::chrono::milliseconds "
                "monitorPeriod(std::chrono::milliseconds stallThreshold), the period the "
                "monitor loop sleeps, floored at 1 ms";
    }
  };
  SmallBus bus;
  check(bus);
}

// ---------------------------------------------------------------------------
// consumerHealth() and healthSnapshot() are advertised as readable at any
// time, and read the bytes the monitor thread writes: _healthBook is plain
// memory, documented as "touched only by the single health-checker thread".
//
// In a plain build these two only have to hold their invariants; under
// ThreadSanitizer they are the reproduction -- but only while the monitor is
// actually writing, which it does on a state transition and never otherwise.
// Hence the consumer held and released on a schedule below, and the
// transition count asserted at the end: a run with no transitions proves
// nothing and looks exactly like a pass.

// Held and released by the test, so the health checker flips the consumer
// between STALLED and HEALTHY on a schedule instead of by luck. A consumer
// publishes its progress once per consume run, so a listener that is merely
// slow inside a long run looks like one uninterrupted stall and the monitor
// writes a state twice in a whole test; being held inside one event and then
// let go is what produces transitions at a known rate.
class Held : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent&) override
  {
    while (hold.load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    count.fetch_add(1, std::memory_order_relaxed);
  }
  std::atomic<bool> hold{false};
  std::atomic<int> count{0};
};

TEST(EventBusHealth, HealthIsReadableWhileTheMonitorWritesIt)
{
  SmallBus bus;
  CallbackRecord transitions;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(4);
  cfg.enableMonitorThread = true;
  cfg.callback = &recordCallback;
  cfg.callbackUser = &transitions;
  bus.setHealthConfig(cfg);

  Held held;
  ASSERT_TRUE(bus.subscribe(&held));
  bus.start();

  std::atomic<bool> run{true};
  std::atomic<uint64_t> reads{0};

  // Ten milliseconds held, ten milliseconds free, against a four millisecond
  // stall threshold: a STALLED and a HEALTHY write every twenty milliseconds.
  std::thread toggler(
      [&held, &run]
      {
        while (run.load(std::memory_order_acquire))
        {
          held.hold.store(true, std::memory_order_release);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          held.hold.store(false, std::memory_order_release);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });

  std::thread publisher(
      [&bus, &run]
      {
        HealthTestEvent ev;
        int i = 0;
        while (run.load(std::memory_order_acquire))
        {
          ev.value = ++i;
          if (bus.publish(ev) < 0)
          {
            break;
          }
        }
      });

  std::thread supervisor(
      [&bus, &run, &reads]
      {
        while (run.load(std::memory_order_acquire))
        {
          // A compiler barrier, nothing more: no instruction is emitted and
          // no thread is synchronised with. It is here because the fields
          // behind consumerHealth() are plain memory, which the optimiser is
          // entitled to read once and keep in a register for the rest of the
          // loop -- and a supervisor that polls health has to read it every
          // time, which is the same reason those fields need to be atomic.
          std::atomic_signal_fence(std::memory_order_acq_rel);
          for (uint32_t i = 0; i < bus.consumerCount(); ++i)
          {
            const auto state = bus.consumerHealth(i);
            EXPECT_TRUE(state == SmallBus::ConsumerHealth::HEALTHY ||
                        state == SmallBus::ConsumerHealth::STALLED ||
                        state == SmallBus::ConsumerHealth::DEAD);
          }
          const auto sweep = bus.healthSnapshot();
          EXPECT_LE(sweep.stalled + sweep.dead, bus.consumerCount());
          reads.fetch_add(1, std::memory_order_relaxed);
          // Paced deliberately. A supervisor polling health in a tight loop
          // hides the very race this test is here for: the race detector
          // folds a run of identical reads from one thread into a single
          // shadow entry, and a monitor write landing in the middle of that
          // run has nothing left to conflict with. Half a millisecond between
          // reads is also what a supervisor actually does.
          std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
      });

  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  run.store(false, std::memory_order_release);
  held.hold.store(false, std::memory_order_release);
  toggler.join();
  publisher.join();
  supervisor.join();
  bus.stop();

  EXPECT_GT(reads.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(bus.healthSnapshot().dead, 0u);
  EXPECT_GT(transitions.stalledEvents.load() + transitions.healthyEvents.load(), 0)
      << "the monitor never wrote a state, so nothing was read against a write";
}

// A state on its own does not tell a supervisor whether a consumer is making
// progress; the sequence and the instant it last moved do, and checkHealth()
// writes those two together. A reader has to get them from the same update:
// lastChange moves only when lastSeen moves, so a report where one advanced
// without the other is a torn read.
// needs: struct ConsumerHealthReport { ConsumerHealth state; int64_t lastSeen;
//        std::chrono::steady_clock::time_point lastChange; };
//        ConsumerHealthReport consumerHealthReport(uint32_t consumerIndex) const
TEST(EventBusHealth, AConsumerHealthReportCarriesOneUpdate)
{
  const auto check = [](auto& bus)
  {
    if constexpr (requires { bus.consumerHealthReport(uint32_t{0}); })
    {
      std::atomic<bool> run{true};
      std::thread publisher(
          [&bus, &run]
          {
            HealthTestEvent ev;
            int i = 0;
            while (run.load(std::memory_order_acquire))
            {
              ev.value = ++i;
              if (bus.publish(ev) < 0)
              {
                break;
              }
            }
          });

      auto previous = bus.consumerHealthReport(0);
      uint64_t moves = 0;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      while (std::chrono::steady_clock::now() < deadline)
      {
        const auto current = bus.consumerHealthReport(0);
        EXPECT_GE(current.lastSeen, previous.lastSeen) << "lastSeen went backwards";
        EXPECT_GE(current.lastChange, previous.lastChange) << "lastChange went backwards";
        const bool seenMoved = current.lastSeen != previous.lastSeen;
        const bool changeMoved = current.lastChange != previous.lastChange;
        EXPECT_EQ(seenMoved, changeMoved)
            << "the sequence and the instant it moved came from different updates";
        moves += seenMoved ? 1 : 0;
        previous = current;
      }

      run.store(false, std::memory_order_release);
      publisher.join();
      bus.stop();
      EXPECT_GT(moves, 0u) << "the consumer never progressed; nothing was observed";
    }
    else
    {
      FAIL() << "needs: ConsumerHealthReport consumerHealthReport(uint32_t) const, "
                "carrying state, lastSeen and lastChange from one update";
    }
  };

  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(5);
  cfg.enableMonitorThread = true;
  bus.setHealthConfig(cfg);
  Counting listener;
  ASSERT_TRUE(bus.subscribe(&listener));
  bus.start();
  check(bus);
}

// What the state in a report is worth is what the pair next to it says. A
// consumer the sweep called STALLED had not moved for at least the stall
// threshold when the sweep looked at it, and the instant it last moved is in
// the same report -- so a report that says STALLED next to an instant that has
// only just passed is two different updates read as one.
TEST(EventBusHealth, AStalledReportIsAtLeastTheStallThresholdOld)
{
  constexpr auto kThreshold = std::chrono::milliseconds(4);
  constexpr auto kHold = std::chrono::milliseconds(6);
  constexpr auto kWindow = std::chrono::milliseconds(3500);
  constexpr int kReaders = 6;

  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = kThreshold;
  cfg.enableMonitorThread = true;
  bus.setHealthConfig(cfg);

  Held held;
  ASSERT_TRUE(bus.subscribe(&held));
  bus.start();

  std::atomic<bool> run{true};

  // Held and released on a schedule, so the sweep writes a state -- both ways
  // round, and the way back carries a fresh pair -- often enough for a reader
  // to be halfway through a report while it does. The readers are several for
  // the same reason: the seam between the state and the version it is checked
  // against is a couple of instructions wide, and the only way at it from
  // outside is to have more than one thread standing in it.
  std::thread toggler(
      [&held, &run, kHold]
      {
        while (run.load(std::memory_order_acquire))
        {
          held.hold.store(true, std::memory_order_release);
          std::this_thread::sleep_for(kHold);
          held.hold.store(false, std::memory_order_release);
          std::this_thread::sleep_for(kHold);
        }
      });

  std::thread publisher(
      [&bus, &run]
      {
        HealthTestEvent ev;
        int i = 0;
        while (run.load(std::memory_order_acquire))
        {
          ev.value = ++i;
          if (bus.publish(ev) < 0)
          {
            break;
          }
        }
      });

  std::atomic<uint64_t> stalledSeen{0};
  std::atomic<uint64_t> tooYoung{0};
  std::atomic<int64_t> youngestUs{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r)
  {
    readers.emplace_back(
        [&bus, &stalledSeen, &tooYoung, &youngestUs, kWindow, kThreshold]
        {
          const auto until = std::chrono::steady_clock::now() + kWindow;
          while (std::chrono::steady_clock::now() < until)
          {
            const auto report = bus.consumerHealthReport(0);
            const auto now = std::chrono::steady_clock::now();
            if (report.state != SmallBus::ConsumerHealth::STALLED)
            {
              continue;
            }
            stalledSeen.fetch_add(1, std::memory_order_relaxed);
            const auto age = now - report.lastChange;
            if (age >= kThreshold)
            {
              continue;
            }
            const auto us =
                std::chrono::duration_cast<std::chrono::microseconds>(age).count();
            if (tooYoung.fetch_add(1, std::memory_order_relaxed) == 0)
            {
              youngestUs.store(us, std::memory_order_relaxed);
            }
          }
        });
  }

  for (auto& r : readers)
  {
    r.join();
  }
  run.store(false, std::memory_order_release);
  held.hold.store(false, std::memory_order_release);
  toggler.join();
  publisher.join();
  bus.stop();

  EXPECT_GT(stalledSeen.load(std::memory_order_relaxed), 0u)
      << "no report ever said STALLED; nothing was checked";
  EXPECT_EQ(tooYoung.load(std::memory_order_relaxed), 0u)
      << tooYoung.load(std::memory_order_relaxed) << " reports said STALLED next to a "
      << "lastChange " << youngestUs.load(std::memory_order_relaxed)
      << " us old, younger than the " << kThreshold.count()
      << " ms it takes to be called stalled: the state and the pair came from "
         "different updates";
}

}  // namespace
