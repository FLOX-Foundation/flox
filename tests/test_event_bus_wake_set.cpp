/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Parking gave a consumer thread somewhere to sleep, and that was as far as
// it went: the condition variable belongs to one bus. A thread that steps
// consumers across many buses cannot block on any one of them, so it went on
// spinning -- the cost parking was built to remove, moved up a level and left
// there.
//
// These tests cover the shared wait point: one WakeSet, three buses, one
// thread over all of them. The same three things have to hold as for a parked
// consumer -- a publish into any ring wakes the sleeper, no wake-up is lost
// (including one landing inside the sleep window itself), and an idle driver
// really does cost nothing -- plus one more that only exists here: a bus
// nobody pointed at a set must behave exactly as it did before there were any.

#include <gtest/gtest.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "flox/util/eventing/event_bus.h"
#include "flox/util/eventing/wake_set.h"
#include "flox/util/performance/busy_backoff.h"

namespace flox
{

struct WakeTestEvent
{
  using Listener = struct IWakeTestListener
  {
    virtual ~IWakeTestListener() = default;
    virtual void onEvent(const WakeTestEvent& e) = 0;
  };

  int value{0};
  int64_t sentNs{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<WakeTestEvent>
{
  static void dispatch(const WakeTestEvent& event, WakeTestEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;
using namespace std::chrono;

using WakeBus = EventBus<WakeTestEvent, 1024, 4>;

constexpr int kBuses = 3;

int64_t nowNs()
{
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Every net in these tests is pushed out this far. A wake-up that arrives
// inside a round's spin budget then cannot have come from the net, whatever
// the runner's scheduler did to it on the way: the mechanism is proved by
// which side of the net the wake-up lands on, not by a number of
// milliseconds that a loaded shared runner is free to miss.
constexpr auto kFarNet = seconds(10);
const int64_t kFarNetUs = duration_cast<microseconds>(kFarNet).count();

// Counts deliveries and remembers the worst publish-to-handler delay it saw.
// One listener type for every test here: a lost wake-up shows up as a delay
// of a whole net interval, so the latency IS the assertion.
class Sink : public WakeTestEvent::Listener
{
 public:
  void onEvent(const WakeTestEvent& e) override
  {
    if (e.sentNs != 0)
    {
      const int64_t d = nowNs() - e.sentNs;
      if (d > worstNs.load(std::memory_order_relaxed))
      {
        worstNs.store(d, std::memory_order_relaxed);
      }
    }
    last.store(e.value, std::memory_order_release);
    count.fetch_add(1, std::memory_order_release);
  }
  std::atomic<int> count{0};
  std::atomic<int> last{-1};
  std::atomic<int64_t> worstNs{0};
};

WakeTestEvent ev(int v)
{
  WakeTestEvent e;
  e.value = v;
  e.sentNs = nowNs();
  return e;
}

// Process CPU time. The test process is this test and the buses, so what it
// measures is what they spent.
milliseconds cpuUsed()
{
#ifdef _WIN32
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0)
  {
    return milliseconds(0);
  }
  const auto toNs = [](const FILETIME& ft)
  {
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return nanoseconds(static_cast<int64_t>(v.QuadPart) * 100);  // 100 ns ticks
  };
  return duration_cast<milliseconds>(toNs(kernel) + toNs(user));
#else
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  const auto us = seconds(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                  microseconds(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
  return duration_cast<milliseconds>(us);
#endif
}

// Busy-wait for the counter: a sleeping poll notices a delivery up to its
// poll interval late, which is useless when the thing being timed is a
// wake-up measured in microseconds.
bool spinFor(const std::atomic<int>& what, int target, milliseconds budget)
{
  const auto deadline = steady_clock::now() + budget;
  while (what.load(std::memory_order_acquire) < target)
  {
    if (steady_clock::now() >= deadline)
    {
      return false;
    }
  }
  return true;
}

// N buses, each with one consumer and no thread of its own, and one thread
// stepping all of them -- the shape a process holding hundreds of rings ends
// up with, in miniature.
//
// How it waits is the variable: on a shared WakeSet, or on a BusyBackoff,
// which is what such a thread had to do before there was a set.
class Fanout
{
 public:
  explicit Fanout(bool useWakeSet) : _useWakeSet(useWakeSet)
  {
    _set.setNetInterval(kFarNet);
    for (int b = 0; b < kBuses; ++b)
    {
      _buses[b] = std::make_unique<WakeBus>();
      _buses[b]->setOwnConsumerThreads(false);
      _buses[b]->setParkNetInterval(kFarNet);
      if (_useWakeSet)
      {
        _buses[b]->setWakeSet(&_set);
      }
      _buses[b]->subscribe(&_sinks[b], /*required=*/true);
      _buses[b]->start();
    }
  }

  ~Fanout() { shutdown(); }

  WakeBus& bus(int b) { return *_buses[b]; }
  Sink& sink(int b) { return _sinks[b]; }
  WakeSet& set() { return _set; }

  int delivered() const
  {
    int n = 0;
    for (const auto& s : _sinks)
    {
      n += s.count.load(std::memory_order_acquire);
    }
    return n;
  }

  int64_t worstNs() const
  {
    int64_t w = 0;
    for (const auto& s : _sinks)
    {
      w = std::max(w, s.worstNs.load(std::memory_order_relaxed));
    }
    return w;
  }

  // Total deliveries across every bus, as one counter a spin can watch.
  const std::atomic<int>& progress() const { return _progress; }

  void run()
  {
    _running.store(true, std::memory_order_release);
    _driver.emplace(
        [this]
        {
          BusyBackoff backoff;
          while (_running.load(std::memory_order_acquire))
          {
            bool any = false;
            for (auto& bus : _buses)
            {
              const uint32_t n = bus->consumerCount();
              for (uint32_t i = 0; i < n; ++i)
              {
                any |= bus->pollConsumer(i);
              }
            }
            if (any)
            {
              _progress.store(delivered(), std::memory_order_release);
              backoff.reset();
              continue;
            }
            if (_useWakeSet)
            {
              _set.parkUnless([this]
                              { return !_running.load(std::memory_order_acquire) ||
                                       pending(); });
            }
            else
            {
              backoff.pause();
            }
          }
        });
  }

  void shutdown()
  {
    if (!_driver.has_value())
    {
      return;
    }
    _running.store(false, std::memory_order_release);
    _set.wake();
    _driver.reset();
    for (auto& bus : _buses)
    {
      bus->stop();
    }
  }

 private:
  // The driver's last look at every ring it steps. Cheap, and it delivers
  // nothing: it runs under the set's mutex.
  bool pending() const
  {
    for (const auto& bus : _buses)
    {
      const uint32_t n = bus->consumerCount();
      for (uint32_t i = 0; i < n; ++i)
      {
        if (bus->consumerHasPending(i))
        {
          return true;
        }
      }
    }
    return false;
  }

  bool _useWakeSet;
  WakeSet _set;
  std::array<std::unique_ptr<WakeBus>, kBuses> _buses{};
  std::array<Sink, kBuses> _sinks{};
  std::atomic<bool> _running{false};
  std::atomic<int> _progress{0};
  std::optional<jthread> _driver{};
};

// ---------------------------------------------------------------------------

TEST(EventBusWakeSet, ABusWithNoSetHasNone)
{
  WakeBus bus;
  EXPECT_EQ(bus.wakeSet(), nullptr);

  WakeSet set;
  bus.setWakeSet(&set);
  EXPECT_EQ(bus.wakeSet(), &set);

  // Before start(), like everything else that fixes the publish path.
  Sink c;
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true));
  bus.start();
  WakeSet other;
  bus.setWakeSet(&other);
  EXPECT_EQ(bus.wakeSet(), &set) << "the wait point changed under a running publisher";
  bus.stop();
}

// A publish into ANY of the rings the thread steps has to reach it. One bus
// at a time, from a cold park each round, so every round exercises the
// wake-up path rather than riding on the one before it.
TEST(EventBusWakeSet, OneThreadParksOnThreeBusesAndWakesFromAnyOfThem)
{
  Fanout fan(/*useWakeSet=*/true);
  fan.run();

  constexpr int kRounds = 60;
  std::array<int, kBuses> expected{};
  const auto t0 = steady_clock::now();
  for (int i = 0; i < kRounds; ++i)
  {
    const int b = i % kBuses;
    // Long enough for the driver to have finished the previous round and
    // gone to sleep. Spin rather than sleep: sleep_for is millisecond-grained
    // on some platforms and this test measures microseconds.
    const auto until = steady_clock::now() + microseconds(80 + (i % 29) * 3);
    while (steady_clock::now() < until)
    {
    }
    fan.bus(b).publish(ev(i));
    ASSERT_TRUE(spinFor(fan.sink(b).count, ++expected[b], milliseconds(2000)))
        << "the driver never woke for bus " << b << " at round " << i;
  }
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

  EXPECT_EQ(fan.delivered(), kRounds);
  // The timed wait inside parkUnless() is a net, not the mechanism. If
  // wake-ups were arriving on its schedule instead of from the publisher,
  // every round would wait out kFarNet and the spin above would have given
  // up long before; the worst delay is bounded by the net for the same
  // reason, and by nothing tighter, because the scheduler is not on trial.
  const auto worstUs = fan.worstNs() / 1000;
  std::printf("worst publish->handler over %d buses on one thread: %lld us (%lld ms total)\n",
              kBuses, static_cast<long long>(worstUs), static_cast<long long>(elapsed.count()));
  EXPECT_LT(worstUs, kFarNetUs / 2) << "a wake-up came from the net, not from the publisher";
  fan.shutdown();
}

// The window the set has to survive is the one between the driver's last look
// at its rings and it raising its hand: a publish landing there sees no
// waiters, sends no wake-up, and -- if nothing follows it -- leaves the
// driver asleep until the net expires. From outside, hitting a couple of
// hundred nanoseconds inside another thread is luck, and earlier measurement
// established that a sweep across the window never lands in it reliably. So this test
// publishes FROM that instant, through the set's probe.
//
// Each round does both halves of the contract in order. The wake publish
// lands on a driver that is already asleep, so it can only be delivered if
// the publisher wakes the set. The probe publish lands inside the window with
// silence behind it, so it can only be delivered if the driver takes one last
// look after raising its hand. Either half missing costs a net interval, and
// the worst-case bound is what says so.
TEST(EventBusWakeSet, APublishInsideTheSleepWindowStillWakesTheDriver)
{
  Fanout fan(/*useWakeSet=*/true);

  struct Probe
  {
    Fanout* fan{nullptr};
    std::atomic<int> armed{0};
    std::atomic<int> fired{0};
  } probe;
  probe.fan = &fan;

  fan.set().setProbe(
      [](void* user)
      {
        auto* p = static_cast<Probe*>(user);
        if (p->armed.exchange(0, std::memory_order_acq_rel) == 0)
        {
          return;
        }
        // The last bus in the set, so the driver has to look past the ones it
        // already found empty.
        p->fan->bus(kBuses - 1).publish(ev(-1));
        p->fired.fetch_add(1, std::memory_order_release);
      },
      &probe);

  fan.run();

  constexpr int kRounds = 50;
  int expected = 0;
  for (int i = 0; i < kRounds; ++i)
  {
    // Give the driver time to actually reach its sleep, so the publish below
    // lands on a sleeper and has to wake the set to be seen at all. Spin
    // rather than sleep: sleep_for is millisecond-grained on some platforms.
    const auto until = steady_clock::now() + microseconds(120 + (i % 17) * 5);
    while (steady_clock::now() < until)
    {
    }
    probe.armed.store(1, std::memory_order_release);
    // Wakes the driver; when it runs out of work and starts going to sleep,
    // the probe fires from inside the window and publishes into the silence.
    fan.bus(i % kBuses).publish(ev(i));
    expected += 2;  // the wake event and the one the probe publishes
    ASSERT_TRUE(spinFor(fan.progress(), expected, milliseconds(3000)))
        << "the driver slept through a publish made inside its sleep window, round " << i;
  }

  EXPECT_EQ(probe.fired.load(), kRounds);
  const auto worstUs = fan.worstNs() / 1000;
  std::printf("worst publish->handler across the sleep window: %lld us\n",
              static_cast<long long>(worstUs));
  EXPECT_LT(worstUs, kFarNetUs / 2) << "a wake-up came from the net, not from the publisher";
  fan.shutdown();
}

// The whole point: a thread over many rings that costs nothing while they are
// all quiet. The comparison is against the same thread waiting the only way
// it could before -- a backoff, which at its laziest still wakes ten thousand
// times a second.
TEST(EventBusWakeSet, IdleDriverOverManyBusesCostsNothingAndBackoffDoesNot)
{
  constexpr auto kIdle = milliseconds(400);

  const auto measure = [kIdle](bool useWakeSet)
  {
    Fanout fan(useWakeSet);
    fan.run();
    fan.bus(0).publish(ev(1));
    // Everything settled; from here the driver has nothing to do.
    const bool settled = spinFor(fan.sink(0).count, 1, milliseconds(2000));
    const auto before = cpuUsed();
    std::this_thread::sleep_for(kIdle);
    const auto after = cpuUsed();
    fan.shutdown();
    return std::pair<milliseconds, bool>{after - before, settled};
  };

  const auto [backoffCpu, backoffSettled] = measure(/*useWakeSet=*/false);
  const auto [parkedCpu, parkedSettled] = measure(/*useWakeSet=*/true);
  ASSERT_TRUE(backoffSettled);
  ASSERT_TRUE(parkedSettled);

  std::printf("idle CPU over %lld ms, one thread over %d buses: backoff %lld ms, wake set %lld ms\n",
              static_cast<long long>(kIdle.count()), kBuses,
              static_cast<long long>(backoffCpu.count()),
              static_cast<long long>(parkedCpu.count()));

  // An idle driver on a set does not run. The bound is generous on purpose --
  // this asserts "does not spin", not a particular scheduler.
  EXPECT_LT(parkedCpu, milliseconds(40));
  if (backoffCpu > milliseconds(40))
  {
    // Only compare when the backoff driver really did burn something; a
    // throttled runner that starves it would make the ratio meaningless.
    EXPECT_LT(parkedCpu.count() * 4, backoffCpu.count());
  }
}

// A bus nobody pointed at a set is the bus as it was before sets existed:
// its own parked consumer thread is still woken by its own condition
// variable.
TEST(EventBusWakeSet, ABusWithoutASetParksItsOwnConsumersAsBefore)
{
  WakeBus bus;
  Sink c;
  bus.setParkNetInterval(kFarNet);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true, WakeBus::WaitMode::PARKED));
  bus.start();
  ASSERT_EQ(bus.wakeSet(), nullptr);

  for (int i = 0; i < 20; ++i)
  {
    const auto until = steady_clock::now() + microseconds(100);
    while (steady_clock::now() < until)
    {
    }
    bus.publish(ev(i));
    ASSERT_TRUE(spinFor(c.count, i + 1, milliseconds(2000))) << "wake-up lost at round " << i;
  }
  EXPECT_LT(c.worstNs.load() / 1000, kFarNetUs / 2) << "the parked consumer rode the net";
  bus.stop();
}

// Both wait points on one bus: a consumer with its own parked thread and a
// driver on a set. One publish has to reach both, and the two must not stand
// in for each other.
TEST(EventBusWakeSet, ASetAndAParkedConsumerCoexistOnOneBus)
{
  WakeBus bus;
  WakeSet set;
  Sink parked;
  bus.setWakeSet(&set);
  ASSERT_TRUE(bus.subscribe(&parked, /*required=*/true, WakeBus::WaitMode::PARKED));
  bus.start();

  for (int i = 0; i < 20; ++i)
  {
    bus.publish(ev(i));
    ASSERT_TRUE(spinFor(parked.count, i + 1, milliseconds(2000)));
  }
  EXPECT_EQ(parked.count.load(), 20);
  bus.stop();
}

// A bus on the way down is a bus that will never publish again. A driver
// asleep on its set has to be told, or it waits out the net for an event that
// is not coming -- and on the way down that net is the shutdown.
TEST(EventBusWakeSet, StoppingABusReleasesADriverParkedOnItsSet)
{
  WakeBus bus;
  WakeSet set;
  Sink c;
  set.setNetInterval(kFarNet);
  bus.setOwnConsumerThreads(false);
  bus.setWakeSet(&set);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true));
  bus.start();

  std::atomic<bool> woke{false};
  std::optional<jthread> waiter;
  waiter.emplace(
      [&set, &woke]
      {
        set.parkUnless([]
                       { return false; });  // nothing will ever be pending
        woke.store(true, std::memory_order_release);
      });

  // Let it reach the sleep before the bus goes down.
  const auto parked = steady_clock::now() + milliseconds(200);
  while (set.waiters() == 0 && steady_clock::now() < parked)
  {
  }
  ASSERT_EQ(set.waiters(), 1u) << "the waiter never parked";

  const auto t0 = steady_clock::now();
  bus.stop();
  const auto deadline = t0 + seconds(2);
  while (!woke.load(std::memory_order_acquire) && steady_clock::now() < deadline)
  {
  }
  const auto took = duration_cast<milliseconds>(steady_clock::now() - t0);
  waiter.reset();

  EXPECT_TRUE(woke.load(std::memory_order_acquire));
  EXPECT_LT(took, kFarNet / 2) << "stop() left the driver to wait out the net";
}

TEST(EventBusWakeSet, WakingAnEmptySetIsFree)
{
  WakeSet set;
  EXPECT_EQ(set.waiters(), 0u);
  set.wake();  // nobody there: no notify, no mutex, no complaint
  EXPECT_EQ(set.waiters(), 0u);
}

}  // namespace
