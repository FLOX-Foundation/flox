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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>

#include "flox/engine/abstract_subsystem.h"
#include "flox/engine/engine_config.h"
#include "flox/engine/event_dispatcher.h"
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

  struct ConsumerSlot
  {
    Listener* listener{nullptr};
    bool required{true};                       // influence on gating
    alignas(64) std::atomic<int64_t> seq{-1};  // last handled seq
    std::optional<std::jthread> thread{};
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

  void subscribe(Listener* listener, bool required = true)
  {
    assert(listener && "Listener must not be null");
    const uint32_t idx = _consumerCount.fetch_add(1, std::memory_order_acq_rel);
    assert(idx < MaxConsumers && "MaxConsumers limit exceeded");
    _consumers[idx].listener = listener;
    _consumers[idx].required = required;
    _consumers[idx].seq.store(-1, std::memory_order_relaxed);
    _gating[idx].store(required ? -1 : INT64_MAX, std::memory_order_relaxed);
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
      auto required = _consumers[i].required;

      _consumers[i].thread.emplace([this, i, l, required]
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
             const auto coreId = targetCores[0];
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
             threadCpuAffinity->pinToCore(assignment.marketDataCores[0]);
             threadCpuAffinity->setRealTimePriority(config::FALLBACK_REALTIME_PRIORITY);
           }
         }
#endif
         {
           std::lock_guard<std::mutex> lk(_readyMutex);
           if (_active.fetch_sub(1, std::memory_order_acq_rel) == 1) _cv.notify_one();
         }
 
         BusyBackoff backoff;
         int64_t next = -1;
 
         while (_running.load(std::memory_order_acquire))
         {
           const int64_t seq = next + 1;
           const size_t  idx = size_t(seq) & Mask;
 
           while (_published[idx].load(std::memory_order_acquire) != seq)
           {
             if (!_running.load(std::memory_order_relaxed)) break;
             backoff.pause();
           }
           if (!_running.load(std::memory_order_relaxed)) break;
 
           if (!required && _constructed[idx].load(std::memory_order_acquire) == 0)
           {
             // skip
           }
           else
           {
             FLOX_PROFILE_SCOPE("Disruptor::deliver");
             EventDispatcher<Event>::dispatch(slot_ref(idx), *l);
           }
 
           _consumers[i].seq.store(seq, std::memory_order_release);
           _gating[i].store(required ? seq : INT64_MAX, std::memory_order_release);
 
           tryReclaim();

           next = seq;
           backoff.reset();
         }
 
         if (_drainOnStop)
         {
           int64_t seq = _consumers[i].seq.load(std::memory_order_relaxed);
           for (;;)
           {
             const int64_t want = seq + 1;
             const size_t  idx  = size_t(want) & Mask;
             if (_published[idx].load(std::memory_order_acquire) != want) break;
 
             if (!required && _constructed[idx].load(std::memory_order_acquire) == 0)
             {
               // skip
             }
             else
             {
               FLOX_PROFILE_SCOPE("Disruptor::drain_deliver");
               EventDispatcher<Event>::dispatch(slot_ref(idx), *l);
             }
 
             _consumers[i].seq.store(want, std::memory_order_release);
             _gating[i].store(required ? want : INT64_MAX, std::memory_order_release);

             tryReclaim();

             seq = want;
           }
         } });
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
  }

  int64_t publish(const Event& ev) { return do_publish(ev); }
  int64_t publish(Event&& ev) { return do_publish(std::move(ev)); }

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
  template <typename Ev>
  int64_t do_publish(Ev&& ev)
  {
    FLOX_PROFILE_SCOPE("Disruptor::publish");

    const int64_t seq = _next.fetch_add(1, std::memory_order_acq_rel) + 1;
    const int64_t wrap = seq - static_cast<int64_t>(CapacityPow2);

    BusyBackoff bo;
    int64_t cachedMin = _cachedMin.load(std::memory_order_relaxed);
    while (wrap > cachedMin)
    {
      cachedMin = minGating();
      _cachedMin.store(cachedMin, std::memory_order_relaxed);
      if (wrap <= cachedMin)
      {
        break;
      }
      bo.pause();
    }

    const size_t idx = size_t(seq) & Mask;

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

    tryReclaim();

    return seq;
  }

  int64_t minGating() const
  {
    const uint32_t n = _consumerCount.load(std::memory_order_acquire);
    int64_t mn = INT64_MAX;
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t s = _gating[i].load(std::memory_order_acquire);
      mn = s < mn ? s : mn;
    }
    return (mn == INT64_MAX) ? _next.load(std::memory_order_acquire) : mn;
  }

  inline void tryReclaim()
  {
    const int64_t upto = minGating();
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

  using Storage = std::aligned_storage_t<sizeof(Event), alignof(Event)>;
  alignas(64) std::array<Storage, CapacityPow2> _storage{};
  inline Event* slot_ptr(size_t idx) noexcept { return std::launder(reinterpret_cast<Event*>(&_storage[idx])); }
  inline Event& slot_ref(size_t idx) noexcept { return *slot_ptr(idx); }

  alignas(64) std::array<std::atomic<int64_t>, CapacityPow2> _published{};
  alignas(64) std::array<std::atomic<uint8_t>, CapacityPow2> _constructed{};

  alignas(64) std::atomic<int64_t> _reclaimSeq{-1};
  alignas(64) std::atomic_flag _reclaimLock = ATOMIC_FLAG_INIT;

  alignas(64) std::array<ConsumerSlot, MaxConsumers> _consumers{};
  alignas(64) std::array<std::atomic<int64_t>, MaxConsumers> _gating{};
  alignas(64) std::atomic<uint32_t> _consumerCount{0};

  std::condition_variable _cv;
  std::mutex _readyMutex;
  std::atomic<uint32_t> _active{0};

  bool _drainOnStop{false};

#if FLOX_CPU_AFFINITY_ENABLED
  // CPU affinity / RT
  std::unique_ptr<performance::CpuAffinity> _cpuAffinity;
  std::optional<performance::CoreAssignment> _coreAssignment;
  std::optional<AffinityConfig> _affinityConfig;
#endif
};

}  // namespace flox
