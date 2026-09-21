/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * What a thread over many shards costs while they are quiet, and how fast it
 * notices when they stop being quiet.
 *
 * A shard with no threads of its own is stepped by somebody. That somebody
 * cannot block on any one shard without going deaf to the rest, so before the
 * shard joined a WakeSet it had two options and both were a price: spin --
 * a whole core, idle, forever -- or run a clock of its own and hop, which
 * pays the same bill in latency instead. The two variants below are that
 * thread with and without the set; the idle column is what the set buys and
 * the wake column is what it costs.
 *
 * The numbers to compare against are the bus-level ones from
 * BM_EventBus_FanoutWakeLatency_{Backoff,WakeSet} -- the same shape one layer
 * down, without the matching engine and the journal in the path.
 *
 * Usage: bench_venue_shard_wake_set [shards] [wake-rounds] [idle-ms]
 */
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "flox/util/concurrency/thread_body.h"
#include "flox/util/eventing/wake_set.h"
#include "flox/util/performance/busy_backoff.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::venue;
using namespace std::chrono;

namespace
{

// Small rings: this measures a wait, not throughput, and 16 shards with the
// default rings would be gigabytes of ring nobody publishes into.
using Shard = SequencedShard<MatchingBook, 1 << 12, 1 << 12>;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg(SymbolId sym)
{
  venue::SymbolConfig c;
  c.id = sym;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = 0;
  c.quoteAsset = 1;
  return c;
}

NewOrder limit(SymbolId sym, OrderId id, double p)
{
  NewOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::SELL;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(1.0);
  o.accountId = 1;
  return o;
}

int64_t nowNs()
{
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Process CPU time. The process is this benchmark and its shards, so what it
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

// Stamps the moment the outbound event reached a handler, which is the far
// end of the wake-up: ingress publish, driver woken, command matched, engine
// event published, handler entered.
struct Sink : IEngineEventListener
{
  std::atomic<int64_t> gotNs{0};
  void onEngineEvent(const EngineEventMsg&) override
  {
    if (gotNs.load(std::memory_order_relaxed) == 0)
    {
      gotNs.store(nowNs(), std::memory_order_release);
    }
  }
};

struct Run
{
  double idleCores{0};
  double wakeMeanNs{0};
  int64_t wakeWorstNs{0};
};

Run measure(int shards, bool useWakeSet, int rounds, int idleMs)
{
  const std::string dir =
      (std::filesystem::temp_directory_path() / "flox_bench_shard_wake_set").string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  WakeSet set;
  std::vector<std::unique_ptr<Shard>> sh;
  std::vector<std::unique_ptr<Sink>> sinks;
  sh.reserve(size_t(shards));
  sinks.reserve(size_t(shards));
  for (int i = 0; i < shards; ++i)
  {
    const std::string base = dir + "/s" + std::to_string(i) + ".bin";
    sh.push_back(std::make_unique<Shard>(cfg(SymbolId(i + 1)), base, MatchingBook{},
                                         Journal::Sync::Off));
    sinks.push_back(std::make_unique<Sink>());
    sh[size_t(i)]->setOwnThreads(false);
    if (useWakeSet)
    {
      sh[size_t(i)]->setWakeSet(&set);
    }
    sh[size_t(i)]->subscribeOutbound(sinks[size_t(i)].get());
    sh[size_t(i)]->start();
  }

  // No cadences on purpose: sweepOnce() is not called, so nothing but a
  // submit can give this thread anything to do. That is what makes the idle
  // column readable -- a driver that sweeps is a driver that is awake because
  // it asked to be.
  std::atomic<bool> running{true};
  auto driver = makeThread(
      "bench.venue.shard_wake_set.driver",
      [&sh, &set, &running, useWakeSet]
      {
        BusyBackoff backoff;
        const auto pending = [&sh]
        {
          for (const auto& s : sh)
          {
            if (s->hasPending())
            {
              return true;
            }
          }
          return false;
        };
        while (running.load(std::memory_order_acquire))
        {
          bool any = false;
          for (auto& s : sh)
          {
            any = s->pollOnce() || any;
          }
          if (any)
          {
            backoff.reset();
            continue;
          }
          if (useWakeSet)
          {
            set.parkUnless([&]
                           { return !running.load(std::memory_order_acquire) || pending(); });
          }
          else
          {
            backoff.pause();
          }
        }
      });

  Run r;

  // ---- what the thread costs while every shard is quiet ----
  sh[0]->submit(InboundCommand{limit(1, 1, 100.0)});
  while (sinks[0]->gotNs.load(std::memory_order_acquire) == 0)
  {
  }
  const auto cpuBefore = cpuUsed();
  const auto wallBefore = steady_clock::now();
  std::this_thread::sleep_for(milliseconds(idleMs));
  const auto wallMs = duration_cast<milliseconds>(steady_clock::now() - wallBefore).count();
  const auto cpuMs = (cpuUsed() - cpuBefore).count();
  r.idleCores = wallMs > 0 ? double(cpuMs) / double(wallMs) : 0.0;

  // ---- how fast it notices ----
  int64_t total = 0;
  OrderId id = 2;
  for (int i = 0; i < rounds; ++i)
  {
    const int b = i % shards;
    // Long enough for the driver to be asleep on the set, or at the far end
    // of its backoff. Outside the measured window on purpose.
    std::this_thread::sleep_for(milliseconds(2));
    sinks[size_t(b)]->gotNs.store(0, std::memory_order_release);

    const int64_t t0 = nowNs();
    sh[size_t(b)]->submit(InboundCommand{limit(SymbolId(b + 1), id++, 100.0)});
    int64_t got = 0;
    while ((got = sinks[size_t(b)]->gotNs.load(std::memory_order_acquire)) == 0)
    {
    }
    const int64_t d = got - t0;
    total += d;
    r.wakeWorstNs = d > r.wakeWorstNs ? d : r.wakeWorstNs;
  }
  r.wakeMeanNs = rounds > 0 ? double(total) / double(rounds) : 0.0;

  running.store(false, std::memory_order_release);
  set.wake();
  driver.join();
  for (auto& s : sh)
  {
    s->stop();
  }
  sh.clear();
  std::filesystem::remove_all(dir, ec);
  return r;
}

}  // namespace

int main(int argc, char** argv)
{
  const int rounds = argc > 2 ? std::atoi(argv[2]) : 300;
  const int idleMs = argc > 3 ? std::atoi(argv[3]) : 400;

  std::vector<int> ks;
  if (argc > 1)
  {
    ks.push_back(std::atoi(argv[1]));
  }
  else
  {
    ks = {3, 16};
  }

  std::printf("one thread over N shards with no threads of their own, no cadences\n");
  std::printf("idle window %d ms, %d wake rounds after 2 ms of quiet each\n\n", idleMs, rounds);
  std::printf("%7s  %10s  %12s  %14s  %14s\n", "shards", "wait", "idle cores", "wake mean ns",
              "wake worst ns");
  for (int k : ks)
  {
    for (bool useSet : {false, true})
    {
      const Run r = measure(k, useSet, rounds, idleMs);
      std::printf("%7d  %10s  %12.3f  %14.0f  %14lld\n", k, useSet ? "wake set" : "backoff",
                  r.idleCores, r.wakeMeanNs, static_cast<long long>(r.wakeWorstNs));
    }
  }
  return 0;
}
