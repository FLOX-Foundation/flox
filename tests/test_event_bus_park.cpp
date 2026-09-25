/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A consumer never blocked: at its laziest it woke ten thousand times a
// second to find the ring still empty, which costs about a seventh of a core
// per consumer and is the right trade only when the machine has cores to
// spare. These tests cover the other mode -- block, and let the publisher
// wake you -- and the three things that make it trustworthy: nothing is
// delivered late, no wake-up is lost, and an idle parked consumer really does
// cost nothing.

#include <gtest/gtest.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

struct ParkTestEvent
{
  using Listener = struct IParkTestListener
  {
    virtual ~IParkTestListener() = default;
    virtual void onEvent(const ParkTestEvent& e) = 0;
  };

  int value{0};
  int64_t sentNs{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<ParkTestEvent>
{
  static void dispatch(const ParkTestEvent& event, ParkTestEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;
using namespace std::chrono;

using ParkBus = EventBus<ParkTestEvent, 1024, 4>;

// Every net in these tests is pushed out this far. A wake-up that arrives
// inside a round's spin budget then cannot have come from the net, whatever
// the runner's scheduler did to it on the way: the mechanism is proved by
// which side of the net the wake-up lands on, not by a number of
// milliseconds that a loaded shared runner is free to miss.
constexpr auto kFarNet = seconds(10);
const int64_t kFarNetUs = duration_cast<microseconds>(kFarNet).count();

int64_t nowNs()
{
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

class Counting : public ParkTestEvent::Listener
{
 public:
  void onEvent(const ParkTestEvent& e) override
  {
    last.store(e.value, std::memory_order_release);
    count.fetch_add(1, std::memory_order_release);
  }
  std::atomic<int> count{0};
  std::atomic<int> last{-1};
};

// Records the worst publish-to-handler delay it ever saw.
class WorstLatency : public ParkTestEvent::Listener
{
 public:
  void onEvent(const ParkTestEvent& e) override
  {
    const int64_t d = nowNs() - e.sentNs;
    if (d > worstNs.load(std::memory_order_relaxed))
    {
      worstNs.store(d, std::memory_order_relaxed);
    }
    count.fetch_add(1, std::memory_order_release);
  }
  std::atomic<int64_t> worstNs{0};
  std::atomic<int> count{0};
};

ParkTestEvent ev(int v)
{
  ParkTestEvent e;
  e.value = v;
  return e;
}

// Process CPU time. The test process is this test and the bus, so what it
// measures is what the bus spent.
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

// Busy-wait for the counter: the sleeping version below notices a delivery
// up to its poll interval late, which is useless when the thing being timed
// is what happens in the few hundred nanoseconds AFTER that delivery.
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

// ---------------------------------------------------------------------------

TEST(EventBusPark, ParkedConsumerDeliversLikeAnyOther)
{
  ParkBus bus;
  Counting c;
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true, ParkBus::WaitMode::PARKED));
  bus.start();

  for (int i = 0; i < 10; ++i)
  {
    bus.publish(ev(i));
  }
  bus.flush();
  EXPECT_EQ(c.count.load(), 10);
  EXPECT_EQ(c.last.load(), 9);
  bus.stop();
}

TEST(EventBusPark, ActiveAndParkedConsumersShareOneBus)
{
  ParkBus bus;
  Counting active;
  Counting parked;
  ASSERT_TRUE(bus.subscribe(&active, /*required=*/true, ParkBus::WaitMode::ACTIVE));
  ASSERT_TRUE(bus.subscribe(&parked, /*required=*/true, ParkBus::WaitMode::PARKED));
  bus.start();

  for (int i = 0; i < 25; ++i)
  {
    bus.publish(ev(i));
  }
  bus.flush();
  EXPECT_EQ(active.count.load(), 25);
  EXPECT_EQ(parked.count.load(), 25);
  bus.stop();
}

// The race every parked consumer has: the ring looks empty, and the publish
// lands in the instant between looking and sleeping. Each round here gives
// the consumer time to actually park and then publishes once, so the wake-up
// path is exercised from a cold start every time; the jitter walks the
// publish across the window where the consumer is deciding to sleep.
TEST(EventBusPark, NoWakeUpIsLost)
{
  ParkBus bus;
  Counting c;
  bus.setParkNetInterval(kFarNet);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true, ParkBus::WaitMode::PARKED));
  bus.start();

  constexpr int kRounds = 300;
  microseconds worst{0};
  const auto t0 = steady_clock::now();
  for (int i = 0; i < kRounds; ++i)
  {
    // Spin rather than sleep: sleep_for's granularity is milliseconds on some
    // platforms, and this test measures microseconds.
    const auto until = steady_clock::now() + microseconds(50 + (i % 37) * 3);
    while (steady_clock::now() < until)
    {
    }
    const auto sent = steady_clock::now();
    bus.publish(ev(i));
    ASSERT_TRUE(spinFor(c.count, i + 1, milliseconds(2000))) << "wake-up lost at round " << i;
    const auto took = duration_cast<microseconds>(steady_clock::now() - sent);
    worst = took > worst ? took : worst;
  }
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

  EXPECT_EQ(c.count.load(), kRounds);
  // The timed wait inside park() is a net, not the mechanism. If wake-ups
  // were arriving on its schedule instead of from the publisher, every round
  // would wait out kFarNet and the spin above would have given up long
  // before; the worst delay is bounded by the net for the same reason, and
  // by nothing tighter, because the scheduler is not on trial.
  std::printf("%d wake-ups from a cold park: worst %lld us, %lld ms total\n", kRounds,
              static_cast<long long>(worst.count()), static_cast<long long>(elapsed.count()));
  EXPECT_LT(worst, kFarNet / 2) << "worst single wake-up came from the net";
  bus.stop();
}

// The window a parked consumer has to survive is the one between its last
// empty look at the ring and raising its hand: a publish that lands there
// sees nobody waiting, sends no wake-up, and -- if nothing else is published
// after it -- leaves the consumer asleep until the net expires.
//
// Continuous publishing does NOT test this: the next event repairs the miss
// by waking the consumer for both. So each round here lands one event in the
// window and then goes quiet, which is the only shape where a lost wake-up
// has consequences. The offset sweeps across the window, because its exact
// position is a property of the machine, not something a test can know.
TEST(EventBusPark, AWakeUpLandingExactlyOnTheSleepIsNotLost)
{
  ParkBus bus;
  WorstLatency sink;
  bus.setParkNetInterval(kFarNet);
  ASSERT_TRUE(bus.subscribe(&sink, /*required=*/true, ParkBus::WaitMode::PARKED));
  bus.start();

  constexpr int kRounds = 800;
  int expected = 0;
  for (int i = 0; i < kRounds; ++i)
  {
    // Wake the consumer and let it finish: after this it polls once more,
    // finds nothing, and starts parking. The window opens here.
    ParkTestEvent wake = ev(i);
    wake.sentNs = nowNs();
    bus.publish(wake);
    // Spin, so the window below is measured from the delivery itself and not
    // from whenever a sleeping poll got round to noticing it.
    ASSERT_TRUE(spinFor(sink.count, ++expected, milliseconds(2000)));

    // Sweep the publish across the window, then go silent.
    const int64_t until = nowNs() + (i % 50) * 100;
    while (nowNs() < until)
    {
    }
    ParkTestEvent probe = ev(i);
    probe.sentNs = nowNs();
    bus.publish(probe);
    ASSERT_TRUE(spinFor(sink.count, ++expected, milliseconds(2000)))
        << "consumer never woke at round " << i;

    if (sink.worstNs.load() > duration_cast<nanoseconds>(kFarNet / 2).count())
    {
      break;  // already failed; do not spend a net interval a round proving it again
    }
  }
  bus.stop();

  const auto worstUs = sink.worstNs.load() / 1000;
  std::printf("worst publish->handler across the sleep window: %lld us\n",
              static_cast<long long>(worstUs));
  // The bound is the net: a lost wake-up shows up as a whole net interval,
  // nothing else does, and with the net at kFarNet the spin above has already
  // given up on any round that rode it.
  EXPECT_LT(worstUs, kFarNetUs / 2) << "a wake-up landed on the sleep and the net picked it up";
}

// The sweep above never reliably lands in the window; from outside, hitting
// a couple of hundred nanoseconds inside another thread is luck. So this one
// publishes FROM that instant, through the park probe: the event is stamped
// after the consumer's last look at the ring and before it raises its hand,
// which is the precise state a lost wake-up needs. Nothing is published
// afterwards, so a miss can only be repaired by the net -- and the assertion
// is that it is not.
TEST(EventBusPark, APublishInsideTheSleepWindowStillWakesTheConsumer)
{
  ParkBus bus;
  WorstLatency sink;
  bus.setParkNetInterval(kFarNet);
  ASSERT_TRUE(bus.subscribe(&sink, /*required=*/true, ParkBus::WaitMode::PARKED));

  struct Probe
  {
    ParkBus* bus{nullptr};
    std::atomic<int> armed{0};
    std::atomic<int> fired{0};
  } probe;
  probe.bus = &bus;

  bus.setParkProbe(
      [](void* user)
      {
        auto* p = static_cast<Probe*>(user);
        if (p->armed.exchange(0, std::memory_order_acq_rel) == 0)
        {
          return;
        }
        ParkTestEvent e;
        e.value = 1;
        e.sentNs = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        p->bus->publish(e);
        p->fired.fetch_add(1, std::memory_order_release);
      },
      &probe);

  bus.start();

  constexpr int kRounds = 50;
  int expected = 0;
  for (int i = 0; i < kRounds; ++i)
  {
    probe.armed.store(1, std::memory_order_release);
    // Wakes the consumer; when it goes back to sleep the probe fires from
    // inside the window and publishes one event into the silence.
    ParkTestEvent wake = ev(i);
    wake.sentNs = nowNs();
    bus.publish(wake);
    expected += 2;  // the wake event and the one the probe publishes
    ASSERT_TRUE(spinFor(sink.count, expected, milliseconds(2000)))
        << "the consumer slept through a publish made inside its sleep window, round " << i;
  }

  bus.stop();
  EXPECT_EQ(probe.fired.load(), kRounds);
  const auto worstUs = sink.worstNs.load() / 1000;
  std::printf("worst publish->handler from inside the sleep window: %lld us\n",
              static_cast<long long>(worstUs));
  EXPECT_LT(worstUs, kFarNetUs / 2) << "the wake-up came from the net, not from the publisher";
}

TEST(EventBusPark, ParkedIdleCostsNothingAndActiveDoesNot)
{
  constexpr auto kIdle = milliseconds(400);

  const auto measure = [kIdle](ParkBus::WaitMode mode)
  {
    ParkBus bus;
    Counting c;
    bus.subscribe(&c, /*required=*/true, mode);
    bus.start();
    bus.publish(ev(1));
    bus.flush();  // everything settled; from here the consumer has nothing to do

    const auto before = cpuUsed();
    std::this_thread::sleep_for(kIdle);
    const auto after = cpuUsed();
    bus.stop();
    return after - before;
  };

  const auto activeCpu = measure(ParkBus::WaitMode::ACTIVE);
  const auto parkedCpu = measure(ParkBus::WaitMode::PARKED);

  std::printf("idle CPU over %lld ms: active %lld ms, parked %lld ms\n",
              static_cast<long long>(kIdle.count()), static_cast<long long>(activeCpu.count()),
              static_cast<long long>(parkedCpu.count()));

  // An idle parked consumer does not run. The bound is generous on purpose --
  // this asserts "does not spin", not a particular scheduler.
  EXPECT_LT(parkedCpu, milliseconds(40));
  if (activeCpu > milliseconds(40))
  {
    // Only compare when the active consumer really did burn something; a
    // throttled runner that starves it would make the ratio meaningless.
    EXPECT_LT(parkedCpu.count() * 4, activeCpu.count());
  }
}

TEST(EventBusPark, StopDoesNotWaitOutTheNet)
{
  ParkBus bus;
  Counting c;
  bus.setParkNetInterval(kFarNet);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true, ParkBus::WaitMode::PARKED));
  bus.start();
  bus.publish(ev(1));
  bus.flush();
  std::this_thread::sleep_for(milliseconds(5));  // let it park

  const auto t0 = steady_clock::now();
  bus.stop();
  const auto took = duration_cast<milliseconds>(steady_clock::now() - t0);
  EXPECT_LT(took, kFarNet / 2) << "stop() waited for the parked consumer's net";
}

TEST(EventBusPark, DrainOnStopStillReachesAParkedConsumer)
{
  ParkBus bus;
  Counting c;
  bus.enableDrainOnStop();
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true, ParkBus::WaitMode::PARKED));
  bus.start();
  for (int i = 0; i < 4; ++i)
  {
    bus.publish(ev(i));
  }
  bus.stop();
  EXPECT_EQ(c.count.load(), 4);
}

}  // namespace
