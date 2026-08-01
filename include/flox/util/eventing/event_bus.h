/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>

#include "flox/engine/abstract_subsystem.h"

// ThreadSanitizer does not model std::atomic_thread_fence, so the
// fence-plus-relaxed publication in publishBatch reads as a race to it.
// Under TSan we fall back to per-slot release stores: same correctness,
// slightly slower, and TSan can see the happens-before edge.
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(thread_sanitizer) || defined(__SANITIZE_THREAD__)
#define FLOX_TSAN_ENABLED 1
#else
#define FLOX_TSAN_ENABLED 0
#endif
#include "flox/engine/engine_config.h"
#include "flox/engine/event_dispatcher.h"
#include "flox/util/concurrency/jthread.h"
#include "flox/util/memory/pool.h"
#include "flox/util/performance/busy_backoff.h"
#include "flox/util/performance/profile.h"
#if FLOX_CPU_AFFINITY_ENABLED
#include "flox/util/performance/cpu_affinity.h"
#endif

namespace flox
{

template <typename T>
struct ListenerType
{
  using type = typename T::Listener;
};

template <typename T>
struct ListenerType<pool::Handle<T>>
{
  using type = typename T::Listener;
};

template <typename Event,
          size_t CapacityPow2 = config::DEFAULT_EVENTBUS_CAPACITY,
          size_t MaxConsumers = config::DEFAULT_EVENTBUS_MAX_CONSUMERS>
class EventBus : public ISubsystem
{
  static_assert(CapacityPow2 > 0, "Capacity must be > 0");
  static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "Capacity must be power of 2");
  static constexpr size_t Mask = CapacityPow2 - 1;
  // Upper bound on how many contiguous published events a consumer delivers
  // before publishing its progress (see the worker loop batch effect).
  static constexpr size_t kMaxConsumeRun = 1024;

 public:
  using Listener = typename ListenerType<Event>::type;

#if FLOX_CPU_AFFINITY_ENABLED
  enum class ComponentType
  {
    MARKET_DATA,
    EXECUTION,
    STRATEGY,
    RISK,
    GENERAL
  };

  struct AffinityConfig
  {
    ComponentType componentType = ComponentType::GENERAL;
    bool enableRealTimePriority = true;
    int realTimePriority = config::DEFAULT_REALTIME_PRIORITY;
    bool enableNumaAwareness = true;
    bool preferIsolatedCores = true;

    AffinityConfig() = default;
    AffinityConfig(ComponentType t, int prio = config::DEFAULT_REALTIME_PRIORITY)
        : componentType(t), realTimePriority(prio) {}
  };
#endif

  using ConsumerRunnerFn = void (*)(EventBus*, uint32_t, void*, bool, BackoffMode);

  struct ConsumerSlot
  {
    void* listener{nullptr};
    ConsumerRunnerFn runner{nullptr};
    bool required{true};                       // influence on gating
    alignas(64) std::atomic<int64_t> seq{-1};  // last handled seq
    std::optional<jthread> thread{};
    uint32_t coreIndex{0};  // index for core distribution
  };

  enum class PublishResult
  {
    SUCCESS,
    TIMEOUT,
    STOPPED
  };

  struct Stats
  {
    uint64_t published{0};
    uint64_t dropped{0};
    uint64_t consumed{0};
  };

 public:
  EventBus()
#if FLOX_CPU_AFFINITY_ENABLED
      : _cpuAffinity(performance::createCpuAffinity())
#endif
  {
    for (auto& p : _published)
    {
      p.store(-1, std::memory_order_relaxed);
    }
    for (auto& c : _constructed)
    {
      c.store(0, std::memory_order_relaxed);
    }
  }

  ~EventBus() { stop(); }

  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  bool subscribe(Listener* listener, bool required = true)
  {
    return subscribeImpl(listener, &runConsumer<Listener>, required);
  }

  // Subscribe with a concrete type. The consumer loop is instantiated
  // around L, so dispatch is a direct call and the handler inlines into the
  // run over published events. L only needs the handler methods the Event's
  // dispatcher calls; deriving from Listener is not required.
  template <typename L>
  bool subscribeStatic(L* listener, bool required = true)
  {
    return subscribeImpl(listener, &runConsumer<L>, required);
  }

  void start() override
  {
    if (_running.exchange(true, std::memory_order_acq_rel))
    {
      return;
    }

    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    _active.store(n, std::memory_order_relaxed);

    for (uint32_t i = 0; i < n; ++i)
    {
      auto* l = _consumers[i].listener;
      auto runner = _consumers[i].runner;
      auto required = _consumers[i].required;
      auto coreIdx = _consumers[i].coreIndex;
      auto backoffMode = _backoffMode;

      _consumers[i].thread.emplace([this, i, l, runner, required, coreIdx, backoffMode]
                                   {
#if FLOX_CPU_AFFINITY_ENABLED
         auto threadCpuAffinity = performance::createCpuAffinity();
         if (_coreAssignment.has_value() && _affinityConfig.has_value())
         {
           auto& assignment = _coreAssignment.value();
           auto& config     = _affinityConfig.value();
           std::vector<int> targetCores;
           switch (config.componentType)
           {
             case ComponentType::MARKET_DATA: targetCores = assignment.marketDataCores; break;
             case ComponentType::EXECUTION:   targetCores = assignment.executionCores;  break;
             case ComponentType::STRATEGY:    targetCores = assignment.strategyCores;   break;
             case ComponentType::RISK:        targetCores = assignment.riskCores;       break;
             case ComponentType::GENERAL:     targetCores = assignment.generalCores;    break;
           }
           if (!targetCores.empty())
           {
             // Distribute consumers across available cores using round-robin
             const auto coreId = targetCores[coreIdx % targetCores.size()];
             const auto pinned = threadCpuAffinity->pinToCore(coreId);
             if (config.enableRealTimePriority)
             {
               auto pr = config.realTimePriority;
               if (pinned && assignment.hasIsolatedCores &&
                   std::find(assignment.allIsolatedCores.begin(),
                             assignment.allIsolatedCores.end(), coreId) != assignment.allIsolatedCores.end())
               {
                 pr += config::ISOLATED_CORE_PRIORITY_BOOST;
               }
               threadCpuAffinity->setRealTimePriority(pr);
             }
           }
         }
         else if (_coreAssignment.has_value())
         {
           auto& assignment = _coreAssignment.value();
           if (!assignment.marketDataCores.empty())
           {
             // Distribute across market data cores
             const auto coreId = assignment.marketDataCores[coreIdx % assignment.marketDataCores.size()];
             threadCpuAffinity->pinToCore(coreId);
             threadCpuAffinity->setRealTimePriority(config::FALLBACK_REALTIME_PRIORITY);
           }
         }
#endif
         {
           std::lock_guard<std::mutex> lk(_readyMutex);
           if (_active.fetch_sub(1, std::memory_order_acq_rel) == 1) _cv.notify_one();
         }
 
         runner(this, i, l, required, backoffMode); });
    }

    std::unique_lock lk(_readyMutex);
    _cv.wait(lk, [&]
             { return _active.load(std::memory_order_acquire) == 0; });
  }

  void stop() override
  {
    if (!_running.exchange(false, std::memory_order_acq_rel))
    {
      return;
    }

    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
    {
      _consumers[i].thread.reset();
    }

    for (size_t i = 0; i < CapacityPow2; ++i)
    {
      if (_constructed[i].exchange(0, std::memory_order_acq_rel))
      {
        slot_ptr(i)->~Event();
      }
      _published[i].store(-1, std::memory_order_relaxed);
    }
    _reclaimSeq.store(-1, std::memory_order_relaxed);
    _cachedMinConsumed.store(-1, std::memory_order_relaxed);
  }

  int64_t publish(const Event& ev) { return do_publish(ev, std::nullopt).second; }
  int64_t publish(Event&& ev) { return do_publish(std::move(ev), std::nullopt).second; }

  // Publish a contiguous batch: one sequence reservation, one wrap/reclaim
  // wait for the whole range, then a single release fence covering all slot
  // stamps instead of one release store per event. Blocking, like publish().
  // Returns the last sequence, or -1 if the bus is stopped.
  int64_t publishBatch(const Event* evs, size_t count)
  {
    FLOX_PROFILE_SCOPE("Disruptor::publishBatch");

    static_assert(CapacityPow2 >= 2);
    assert(count > 0 && count <= CapacityPow2 / 2 && "batch must fit the ring with room to spare");

    if (!_running.load(std::memory_order_acquire))
    {
      return -1;
    }

    const int64_t lastSeq = _next.fetch_add(static_cast<int64_t>(count),
                                            std::memory_order_acq_rel) +
                            static_cast<int64_t>(count);
    const int64_t firstSeq = lastSeq - static_cast<int64_t>(count) + 1;
    if (firstSeq < 0)
    {
      return -1;
    }

    // Wrap gating for the whole range (required consumers).
    const int64_t wrap = lastSeq - static_cast<int64_t>(CapacityPow2);
    {
      BusyBackoff bo;
      int64_t cachedMin = _cachedMin.load(std::memory_order_acquire);
      while (wrap > cachedMin)
      {
        if (!_running.load(std::memory_order_relaxed))
        {
          return -1;
        }
        cachedMin = minGating();
        _cachedMin.store(cachedMin, std::memory_order_release);
        if (wrap <= cachedMin)
        {
          break;
        }
        bo.pause();
      }
    }

    // Reclaim gating for the whole range (all consumers), through the
    // monotonic cache.
    if (wrap >= 0 && _cachedMinConsumed.load(std::memory_order_acquire) < wrap)
    {
      BusyBackoff reclaimBo;
      int64_t observed;
      while ((observed = minConsumed()) < wrap)
      {
        if (!_running.load(std::memory_order_relaxed))
        {
          return -1;
        }
        reclaimBo.pause();
      }
      _cachedMinConsumed.store(observed, std::memory_order_release);
    }

    for (size_t k = 0; k < count; ++k)
    {
      const int64_t seq = firstSeq + static_cast<int64_t>(k);
      const size_t idx = size_t(seq) & Mask;
      if (_constructed[idx].exchange(0, std::memory_order_acq_rel))
      {
        slot_ptr(idx)->~Event();
      }
      ::new (slot_ptr(idx)) Event(evs[k]);
      auto& obj = slot_ref(idx);
      if constexpr (requires { obj->tickSequence; })
      {
        obj->tickSequence = static_cast<uint64_t>(seq);
      }
      if constexpr (requires { obj.tickSequence; })
      {
        obj.tickSequence = static_cast<uint64_t>(seq);
      }
    }

#if FLOX_TSAN_ENABLED
    for (size_t k = 0; k < count; ++k)
    {
      const int64_t seq = firstSeq + static_cast<int64_t>(k);
      const size_t idx = size_t(seq) & Mask;
      _constructed[idx].store(1, std::memory_order_release);
      _published[idx].store(seq, std::memory_order_release);
    }
#else
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t k = 0; k < count; ++k)
    {
      const int64_t seq = firstSeq + static_cast<int64_t>(k);
      const size_t idx = size_t(seq) & Mask;
      _constructed[idx].store(1, std::memory_order_relaxed);
      _published[idx].store(seq, std::memory_order_relaxed);
    }
#endif

    _publishCount.fetch_add(count, std::memory_order_relaxed);
    return lastSeq;
  }

  // Publish with timeout - returns result and sequence number (-1 on failure).
  // Default 1ms: zero-timeout is an anti-pattern on multi-core due to cache coherency latency.
  std::pair<PublishResult, int64_t> tryPublish(const Event& ev,
                                               std::chrono::microseconds timeout = std::chrono::microseconds{1000})
  {
    return do_publish(ev, timeout);
  }
  std::pair<PublishResult, int64_t> tryPublish(Event&& ev,
                                               std::chrono::microseconds timeout = std::chrono::microseconds{1000})
  {
    return do_publish(std::move(ev), timeout);
  }

  Stats stats() const
  {
    return Stats{
        _publishCount.load(std::memory_order_relaxed),
        _dropCount.load(std::memory_order_relaxed),
        _consumeCount.load(std::memory_order_relaxed)};
  }

  void waitConsumed(int64_t seq)
  {
    FLOX_PROFILE_SCOPE("Disruptor::waitConsumed");
    BusyBackoff bo;
    while (_running.load(std::memory_order_acquire) && minGating() < seq)
    {
      bo.pause();
    }
  }

  void flush()
  {
    const int64_t last = _next.load(std::memory_order_acquire);
    waitConsumed(last);
  }

  uint32_t consumerCount() const { return _consumerCount.load(std::memory_order_acquire); }
  void enableDrainOnStop() { _drainOnStop = true; }

  void setBackoffMode(BackoffMode mode) { _backoffMode = mode; }

#if FLOX_CPU_AFFINITY_ENABLED
  // ---------- CPU Affinity / RT priority ----------
  void setAffinityConfig(const AffinityConfig& cfg)
  {
    _affinityConfig = cfg;

    performance::CriticalComponentConfig coreCfg;
    coreCfg.preferIsolatedCores = cfg.preferIsolatedCores;
    coreCfg.exclusiveIsolatedCores = true;
    coreCfg.allowSharedCriticalCores = false;

    if (cfg.enableNumaAwareness)
    {
      _coreAssignment = _cpuAffinity->getNumaAwareCoreAssignment(coreCfg);
    }
    else
    {
      _coreAssignment = _cpuAffinity->getRecommendedCoreAssignment(coreCfg);
    }
  }

  void setCoreAssignment(const performance::CoreAssignment& assignment)
  {
    _coreAssignment = assignment;
    _affinityConfig = AffinityConfig{ComponentType::GENERAL, config::DEFAULT_REALTIME_PRIORITY};
  }

  std::optional<performance::CoreAssignment> getCoreAssignment() const { return _coreAssignment; }
  std::optional<AffinityConfig> getAffinityConfig() const { return _affinityConfig; }

  bool setupOptimalConfiguration(ComponentType componentType, bool enablePerformanceOptimizations = false)
  {
    AffinityConfig cfg;
    cfg.componentType = componentType;
    cfg.enableRealTimePriority = (componentType != ComponentType::GENERAL);
    cfg.enableNumaAwareness = true;
    cfg.preferIsolatedCores = true;

    switch (componentType)
    {
      case ComponentType::MARKET_DATA:
        cfg.realTimePriority = config::MARKET_DATA_PRIORITY;
        break;
      case ComponentType::EXECUTION:
        cfg.realTimePriority = config::EXECUTION_PRIORITY;
        break;
      case ComponentType::STRATEGY:
        cfg.realTimePriority = config::STRATEGY_PRIORITY;
        break;
      case ComponentType::RISK:
        cfg.realTimePriority = config::RISK_PRIORITY;
        break;
      case ComponentType::GENERAL:
        cfg.realTimePriority = config::GENERAL_PRIORITY;
        break;
    }
    setAffinityConfig(cfg);

    if (enablePerformanceOptimizations)
    {
      _cpuAffinity->disableCpuFrequencyScaling();
    }
    return _coreAssignment.has_value();
  }

  bool verifyIsolatedCoreConfiguration() const
  {
    if (!_coreAssignment.has_value())
    {
      return false;
    }
    return _cpuAffinity->verifyCriticalCoreIsolation(_coreAssignment.value());
  }
#endif

 private:
  bool subscribeImpl(void* listener, ConsumerRunnerFn runner, bool required)
  {
    if (!listener)
    {
      return false;
    }
    if (_running.load(std::memory_order_acquire))
    {
      return false;  // Cannot subscribe after start
    }
    const uint32_t idx = _consumerCount.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= MaxConsumers)
    {
      _consumerCount.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }
    _consumers[idx].listener = listener;
    _consumers[idx].runner = runner;
    _consumers[idx].required = required;
    _consumers[idx].seq.store(-1, std::memory_order_relaxed);
    _consumers[idx].coreIndex = idx;  // Store index for core distribution
    _gating[idx].v.store(required ? -1 : INT64_MAX, std::memory_order_relaxed);
    return true;
  }

  template <typename L>
  static void runConsumer(EventBus* self, uint32_t i, void* obj, bool required, BackoffMode mode)
  {
    self->consumerLoop(i, static_cast<L*>(obj), required, mode);
  }

  // The whole consume loop, monomorphic in the subscriber type. For
  // L = Listener this is exactly the historical virtual path; for a concrete
  // L (via subscribeStatic) dispatch resolves statically and the handler
  // inlines into the batched run.
  template <typename L>
  void consumerLoop(uint32_t i, L* l, bool required, BackoffMode backoffMode)
  {
    BusyBackoff backoff(backoffMode);
    int64_t next = -1;

    while (_running.load(std::memory_order_acquire))
    {
      const int64_t seq = next + 1;
      const size_t idx = size_t(seq) & Mask;

      while (_published[idx].load(std::memory_order_acquire) != seq)
      {
        if (!_running.load(std::memory_order_relaxed))
        {
          break;
        }
        backoff.pause();
      }

      if (!_running.load(std::memory_order_relaxed))
      {
        break;
      }

      // Batch effect: consume the whole contiguous published run and
      // publish progress once at its end, instead of two release stores
      // per event. The run is bounded so producers waiting on the wrap
      // never starve for progress longer than kMaxConsumeRun events.
      int64_t last = next;
      int64_t cur = seq;
      uint64_t delivered = 0;
      size_t run = 0;
      while (run < kMaxConsumeRun)
      {
        const size_t cidx = size_t(cur) & Mask;
        if (_published[cidx].load(std::memory_order_acquire) != cur)
        {
          break;
        }

        // Value 2 = timeout placeholder (event was never constructed in
        // this slot), value 0 = reclaimed, value 1 = valid event. ALL
        // consumers must skip timeout placeholders -- dispatching would
        // read stale/uninitialized memory from a previous wrap-around.
        // Relaxed is enough: the construction store is ordered before the
        // slot stamp acquired above.
        if (_constructed[cidx].load(std::memory_order_relaxed) == 1)
        {
          FLOX_PROFILE_SCOPE("Disruptor::deliver");
          EventDispatcher<Event>::dispatch(slot_ref(cidx), *l);
          ++delivered;
        }

        last = cur;
        ++cur;
        ++run;
      }

      _consumers[i].seq.store(last, std::memory_order_release);
      if (required)
      {
        _gating[i].v.store(last, std::memory_order_release);
      }
      if (delivered != 0)
      {
        _consumeCount.fetch_add(delivered, std::memory_order_relaxed);
      }

      next = last;
      backoff.reset();
    }

    if (_drainOnStop)
    {
      int64_t seq = _consumers[i].seq.load(std::memory_order_relaxed);
      for (;;)
      {
        const int64_t want = seq + 1;
        const size_t idx = size_t(want) & Mask;
        if (_published[idx].load(std::memory_order_acquire) != want)
        {
          break;
        }

        if (_constructed[idx].load(std::memory_order_acquire) == 1)
        {
          FLOX_PROFILE_SCOPE("Disruptor::drain_deliver");
          EventDispatcher<Event>::dispatch(slot_ref(idx), *l);
          _consumeCount.fetch_add(1, std::memory_order_relaxed);
        }

        _consumers[i].seq.store(want, std::memory_order_release);
        _gating[i].v.store(required ? want : INT64_MAX, std::memory_order_release);

        seq = want;
      }
    }
  }

  template <typename Ev>
  std::pair<PublishResult, int64_t> do_publish(Ev&& ev, std::optional<std::chrono::microseconds> timeout)
  {
    FLOX_PROFILE_SCOPE("Disruptor::publish");

    if (!_running.load(std::memory_order_acquire))
    {
      return {PublishResult::STOPPED, -1};
    }

    // Reserve sequence number
    const int64_t seq = _next.fetch_add(1, std::memory_order_acq_rel) + 1;

    // Check for overflow (very unlikely but safe)
    if (seq < 0)
    {
      _next.fetch_sub(1, std::memory_order_acq_rel);
      return {PublishResult::STOPPED, -1};
    }

    const int64_t wrap = seq - static_cast<int64_t>(CapacityPow2);

    BusyBackoff bo;
    int64_t cachedMin = _cachedMin.load(std::memory_order_acquire);

    auto startTime = timeout.has_value() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    while (wrap > cachedMin)
    {
      if (!_running.load(std::memory_order_relaxed))
      {
        return {PublishResult::STOPPED, -1};
      }

      if (timeout.has_value())
      {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed >= timeout.value())
        {
          // Timeout - we already reserved the slot, so mark it as timeout placeholder (value 2)
          // This distinguishes from reclaimed slots (value 0) which optional consumers can skip
          const size_t idx = size_t(seq) & Mask;
          _constructed[idx].store(2, std::memory_order_release);
          _published[idx].store(seq, std::memory_order_release);
          _dropCount.fetch_add(1, std::memory_order_relaxed);
          return {PublishResult::TIMEOUT, -1};
        }
      }

      cachedMin = minGating();
      _cachedMin.store(cachedMin, std::memory_order_release);
      if (wrap <= cachedMin)
      {
        break;
      }
      bo.pause();
    }

    const size_t idx = size_t(seq) & Mask;

    // Wait for ALL consumers (including optional) to process the old event
    // before destroying it. This prevents use-after-free for optional
    // consumers. minConsumed() is a scan over every consumer's progress line,
    // so consult the monotonic cache first: any previously observed lower
    // bound stays valid forever, and the scan runs only when it is not enough.
    const int64_t oldSeq = seq - static_cast<int64_t>(CapacityPow2);
    if (oldSeq >= 0 && _cachedMinConsumed.load(std::memory_order_acquire) < oldSeq)
    {
      BusyBackoff reclaimBo;
      int64_t observed;
      while ((observed = minConsumed()) < oldSeq)
      {
        if (!_running.load(std::memory_order_relaxed))
        {
          return {PublishResult::STOPPED, -1};
        }
        reclaimBo.pause();
      }
      _cachedMinConsumed.store(observed, std::memory_order_release);
    }

    // Destroy old event if present - only if not already reclaimed
    // The _constructed flag ensures only one thread destroys
    if (_constructed[idx].exchange(0, std::memory_order_acq_rel))
    {
      slot_ptr(idx)->~Event();
    }

    ::new (slot_ptr(idx)) Event(std::forward<Ev>(ev));
    _constructed[idx].store(1, std::memory_order_release);

    auto& obj = slot_ref(idx);
    if constexpr (requires { obj->tickSequence; })
    {
      obj->tickSequence = static_cast<uint64_t>(seq);
    }
    if constexpr (requires { obj.tickSequence; })
    {
      obj.tickSequence = static_cast<uint64_t>(seq);
    }

    _published[idx].store(seq, std::memory_order_release);

    _publishCount.fetch_add(1, std::memory_order_relaxed);
    return {PublishResult::SUCCESS, seq};
  }

  int64_t minGating() const
  {
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    int64_t mn = INT64_MAX;
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t s = _gating[i].v.load(std::memory_order_acquire);
      mn = s < mn ? s : mn;
    }
    return (mn == INT64_MAX) ? _next.load(std::memory_order_acquire) : mn;
  }

  // Returns minimum sequence consumed by ALL consumers (including optional)
  // Used for safe reclaim - events can only be destroyed after ALL consumers processed them
  // If no consumers, returns INT64_MAX to indicate all events are "consumed"
  int64_t minConsumed() const
  {
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    if (n == 0)
    {
      return INT64_MAX;  // No consumers = everything is consumed
    }
    int64_t mn = INT64_MAX;
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t s = _consumers[i].seq.load(std::memory_order_acquire);
      mn = s < mn ? s : mn;
    }
    return mn;
  }

  inline void tryReclaim()
  {
    // Use minConsumed() instead of minGating() to ensure optional consumers
    // have processed the event before destruction
    const int64_t upto = minConsumed();
    int64_t cur = _reclaimSeq.load(std::memory_order_relaxed);
    if (upto <= cur)
    {
      return;
    }

    if (_reclaimLock.test_and_set(std::memory_order_acquire))
    {
      return;
    }
    cur = _reclaimSeq.load(std::memory_order_relaxed);
    if (upto > cur)
    {
      for (int64_t s = cur + 1; s <= upto; ++s)
      {
        const size_t idx = size_t(s) & Mask;
        if (_constructed[idx].exchange(0, std::memory_order_acq_rel))
        {
          slot_ptr(idx)->~Event();
        }
      }

      _reclaimSeq.store(upto, std::memory_order_release);
    }

    _reclaimLock.clear(std::memory_order_release);
  }

 private:
  alignas(64) std::atomic<bool> _running{false};
  alignas(64) std::atomic<int64_t> _next{-1};
  alignas(64) std::atomic<int64_t> _cachedMin{-1};

  struct alignas(alignof(Event)) Storage
  {
    std::byte data[sizeof(Event)];
  };
  alignas(64) std::array<Storage, CapacityPow2> _storage{};
  inline Event* slot_ptr(size_t idx) noexcept { return std::launder(reinterpret_cast<Event*>(_storage[idx].data)); }
  inline Event& slot_ref(size_t idx) noexcept { return *slot_ptr(idx); }

  alignas(64) std::array<std::atomic<int64_t>, CapacityPow2> _published{};
  // _constructed values: 0 = empty/reclaimed, 1 = valid event, 2 = timeout placeholder
  alignas(64) std::array<std::atomic<uint8_t>, CapacityPow2> _constructed{};

  // Monotonic lower bound of minConsumed(); stale values are always safe.
  // Release/acquire so a publisher that trusts the cached bound inherits the
  // happens-before edges the publisher that scanned it established.
  alignas(64) std::atomic<int64_t> _cachedMinConsumed{-1};

  alignas(64) std::atomic<int64_t> _reclaimSeq{-1};
  alignas(64) std::atomic_flag _reclaimLock = ATOMIC_FLAG_INIT;

  alignas(64) std::array<ConsumerSlot, MaxConsumers> _consumers{};
  // One cache line per consumer: packed gating atomics false-share between
  // consumers storing progress and the producer scanning it.
  struct alignas(64) PaddedSeq
  {
    std::atomic<int64_t> v{0};
  };
  alignas(64) std::array<PaddedSeq, MaxConsumers> _gating{};
  alignas(64) std::atomic<uint32_t> _consumerCount{0};

  std::condition_variable _cv;
  std::mutex _readyMutex;
  std::atomic<uint32_t> _active{0};

  bool _drainOnStop{false};
  BackoffMode _backoffMode{config::defaultBackoffMode};

  // Monitoring counters (relaxed ordering -- advisory only)
  alignas(64) std::atomic<uint64_t> _publishCount{0};
  alignas(64) std::atomic<uint64_t> _dropCount{0};
  alignas(64) std::atomic<uint64_t> _consumeCount{0};

#if FLOX_CPU_AFFINITY_ENABLED
  // CPU affinity / RT
  std::unique_ptr<performance::CpuAffinity> _cpuAffinity;
  std::optional<performance::CoreAssignment> _coreAssignment;
  std::optional<AffinityConfig> _affinityConfig;
#endif
};

}  // namespace flox
