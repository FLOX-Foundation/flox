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
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "flox/engine/abstract_subsystem.h"
#include "flox/engine/engine_config.h"
#include "flox/engine/event_dispatcher.h"
#include "flox/engine/tick_barrier.h"
#include "flox/engine/tick_guard.h"
#include "flox/util/concurrency/spsc_queue.h"
#include "flox/util/memory/pool.h"
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

template <typename Event>
struct SyncPolicy
{
  using QueueItem = std::pair<Event, TickBarrier*>;
  static QueueItem makeItem(Event ev, TickBarrier* barrier) { return {ev, barrier}; }
  static void dispatch(const QueueItem& item, typename ListenerType<Event>::type& listener)
  {
    TickGuard guard(*item.second);
    EventDispatcher<Event>::dispatch(item.first, listener);
  }
};

template <typename Event>
struct AsyncPolicy
{
  using QueueItem = Event;
  static QueueItem makeItem(Event ev, void*) { return ev; }
  static void dispatch(const QueueItem& item, typename ListenerType<Event>::type& listener)
  {
    EventDispatcher<Event>::dispatch(item, listener);
  }
};

template <typename Event, typename Policy, size_t QueueSize = config::DEFAULT_EVENTBUS_QUEUE_SIZE>
class EventBus : public ISubsystem
{
 public:
  using Listener = typename ListenerType<Event>::type;
  using QueueItem = typename Policy::QueueItem;
  using Queue = SPSCQueue<QueueItem, QueueSize>;

  EventBus()
#if FLOX_CPU_AFFINITY_ENABLED
      : _cpuAffinity(performance::createCpuAffinity())
#endif
  {
  }

  ~EventBus() { stop(); }

  void subscribe(std::shared_ptr<Listener> listener)
  {
    SubscriberMode mode = listener->mode();

    Entry e;
    e.mode = mode;
    e.listener = std::move(listener);
    e.queue = std::make_unique<Queue>();

    std::lock_guard lock(_mutex);
    _subs.emplace(e.listener->id(), std::move(e));
  }

  void start() override
  {
    if (_running.exchange(true))
    {
      return;
    }

    std::lock_guard lock(_mutex);
    _active = 0;

    for (auto& [_, e] : _subs)
    {
      if (e.mode == SubscriberMode::PUSH)
      {
        ++_active;
      }
    }

    for (auto& [_, e] : _subs)
    {
      if (e.mode == SubscriberMode::PUSH)
      {
        auto* queue = e.queue.get();
        auto listener = e.listener;
        e.thread.emplace([this, queue, listener]
                         {
#if FLOX_CPU_AFFINITY_ENABLED
          // Create a local CpuAffinity instance for this thread
          auto threadCpuAffinity = performance::createCpuAffinity();

          // Apply enhanced CPU affinity if configured
          if (_coreAssignment.has_value() && _affinityConfig.has_value())
          {
            auto& assignment = _coreAssignment.value();
            auto& config = _affinityConfig.value();
            
            // Select appropriate cores based on component type
            std::vector<int> targetCores;
            std::string componentName;
            
            switch (config.componentType)
            {
              case ComponentType::MARKET_DATA:
                targetCores = assignment.marketDataCores;
                componentName = "marketData";
                break;
              case ComponentType::EXECUTION:
                targetCores = assignment.executionCores;
                componentName = "execution";
                break;
              case ComponentType::STRATEGY:
                targetCores = assignment.strategyCores;
                componentName = "strategy";
                break;
              case ComponentType::RISK:
                targetCores = assignment.riskCores;
                componentName = "risk";
                break;
              case ComponentType::GENERAL:
                targetCores = assignment.generalCores;
                componentName = "general";
                break;
            }
            
            if (!targetCores.empty())
            {
              const auto coreId = targetCores[0];  // Use first assigned core
              const auto pinned = threadCpuAffinity->pinToCore(coreId);
              
              if (config.enableRealTimePriority)
              {
                auto priority = config.realTimePriority;

                if (pinned && assignment.hasIsolatedCores)
                {
                  const auto isIsolated = std::find(assignment.allIsolatedCores.begin(),
                                                    assignment.allIsolatedCores.end(), coreId)
                                           != assignment.allIsolatedCores.end();
                  if (isIsolated)
                  {
                    priority += config::ISOLATED_CORE_PRIORITY_BOOST;
                  }
                }

                threadCpuAffinity->setRealTimePriority(priority);
              }
            }
          }
          else if (_coreAssignment.has_value())
          {
            // Direct assignment fallback - pin to market data cores as default
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
            if (--_active == 0) _cv.notify_one();
          }
          while (_running.load(std::memory_order_acquire)) {
            if (auto* item = queue->try_pop()) {
              Policy::dispatch(*item, *listener);
              item->~QueueItem();
            } else {
              std::this_thread::yield();
            }
          }
          while (auto* item = queue->try_pop())
          {
            if (_drainOnStop)
            {
              Policy::dispatch(*item, *listener);
            }

            item->~QueueItem();
          } });
      }
    }

    std::unique_lock lk(_readyMutex);
    _cv.wait(lk, [&]
             { return _active == 0; });
  }

  void stop() override
  {
    if (!_running.exchange(false))
    {
      return;
    }
    std::lock_guard lock(_mutex);
    for (auto& [_, e] : _subs)
    {
      e.thread.reset();
      e.queue->clear();
    }
    _subs.clear();
  }

  void publish(Event ev)
  {
    if (!_running.load(std::memory_order_acquire))
    {
      return;
    }

    uint64_t seq = _tickCounter.fetch_add(1, std::memory_order_relaxed);

    if constexpr (requires { ev->tickSequence; })
    {
      ev->tickSequence = seq;
    }
    if constexpr (requires { ev.tickSequence; })
    {
      ev.tickSequence = seq;
    }

    TickBarrier barrier(_subs.size());

    std::lock_guard lock(_mutex);
    for (auto& [_, e] : _subs)
    {
      if constexpr (std::is_same_v<Policy, SyncPolicy<Event>>)
      {
        e.queue->emplace(Policy::makeItem(ev, &barrier));
      }
      else
      {
        e.queue->emplace(Policy::makeItem(ev, nullptr));
      }
    }

    if constexpr (std::is_same_v<Policy, SyncPolicy<Event>>)
    {
      barrier.wait();
    }
  }

  Queue* getQueue(SubscriberId id)
  {
    std::lock_guard lock(_mutex);
    auto it = _subs.find(id);
    if (it != _subs.end() && it->second.mode == SubscriberMode::PULL)
    {
      return it->second.queue.get();
    }
    return nullptr;
  }

  uint64_t currentTickId() const noexcept
  {
    return _tickCounter.load(std::memory_order_relaxed);
  }

  void enableDrainOnStop()
  {
    _drainOnStop = true;
  }

#if FLOX_CPU_AFFINITY_ENABLED
  /**
   * @brief Event bus component types for CPU affinity assignment
   */
  enum class ComponentType
  {
    MARKET_DATA,  // Market data processing (trade events, book updates)
    EXECUTION,    // Order execution and fills
    STRATEGY,     // Strategy computation and signals
    RISK,         // Risk management and validation
    GENERAL       // General purpose / logging / metrics
  };

  /**
   * @brief Configuration for event bus CPU affinity
   */
  struct AffinityConfig
  {
    ComponentType componentType = ComponentType::GENERAL;
    bool enableRealTimePriority = true;
    int realTimePriority = config::DEFAULT_REALTIME_PRIORITY;
    bool enableNumaAwareness = true;
    bool preferIsolatedCores = true;

    AffinityConfig() = default;

    AffinityConfig(ComponentType type, int priority = config::DEFAULT_REALTIME_PRIORITY)
        : componentType(type), realTimePriority(priority) {}
  };
#endif

#if FLOX_CPU_AFFINITY_ENABLED
  /**
   * @brief Configure CPU affinity for event bus threads using enhanced isolated core functionality
   * @param config Affinity configuration including component type and priorities
   */
  void setAffinityConfig(const AffinityConfig& config)
  {
    _affinityConfig = config;

    // Generate optimal core assignment using isolated cores
    performance::CriticalComponentConfig coreConfig;
    coreConfig.preferIsolatedCores = config.preferIsolatedCores;
    coreConfig.exclusiveIsolatedCores = true;
    coreConfig.allowSharedCriticalCores = false;

    if (config.enableNumaAwareness)
    {
      _coreAssignment = _cpuAffinity->getNumaAwareCoreAssignment(coreConfig);
    }
    else
    {
      _coreAssignment = _cpuAffinity->getRecommendedCoreAssignment(coreConfig);
    }
  }

  /**
   * @brief Configure CPU affinity using direct core assignment
   * @param assignment Core assignment configuration
   * @note For advanced use cases. Most users should use setupOptimalConfiguration() instead
   */
  void setCoreAssignment(const performance::CoreAssignment& assignment)
  {
    _coreAssignment = assignment;
    // Set default affinity config for compatibility
    _affinityConfig = AffinityConfig{ComponentType::GENERAL, config::DEFAULT_REALTIME_PRIORITY};
  }

  /**
   * @brief Get current CPU affinity configuration
   * @return Optional core assignment
   */
  std::optional<performance::CoreAssignment> getCoreAssignment() const
  {
    return _coreAssignment;
  }

  /**
   * @brief Get current affinity configuration
   * @return Optional affinity configuration
   */
  std::optional<AffinityConfig> getAffinityConfig() const
  {
    return _affinityConfig;
  }

  /**
   * @brief Setup optimal performance configuration for this event bus
   * @param componentType Type of component this event bus serves
   * @param enablePerformanceOptimizations Enable CPU frequency scaling and other optimizations
   * @return true if setup was successful
   */
  bool setupOptimalConfiguration(ComponentType componentType, bool enablePerformanceOptimizations = false)
  {
    AffinityConfig config;
    config.componentType = componentType;
    config.enableRealTimePriority = true;
    config.enableNumaAwareness = true;
    config.preferIsolatedCores = true;

    // Set component-specific priorities
    switch (componentType)
    {
      case ComponentType::MARKET_DATA:
        config.realTimePriority = config::MARKET_DATA_PRIORITY;
        break;
      case ComponentType::EXECUTION:
        config.realTimePriority = config::EXECUTION_PRIORITY;
        break;
      case ComponentType::STRATEGY:
        config.realTimePriority = config::STRATEGY_PRIORITY;
        break;
      case ComponentType::RISK:
        config.realTimePriority = config::RISK_PRIORITY;
        break;
      case ComponentType::GENERAL:
        config.realTimePriority = config::GENERAL_PRIORITY;
        config.enableRealTimePriority = false;  // Don't use RT priority for general
        break;
    }

    setAffinityConfig(config);

    if (enablePerformanceOptimizations)
    {
      // Optionally disable frequency scaling for performance
      _cpuAffinity->disableCpuFrequencyScaling();
    }

    return _coreAssignment.has_value();
  }

  /**
   * @brief Verify that this event bus is properly configured for isolated cores
   * @return true if using isolated cores optimally
   */
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
  struct Entry
  {
    std::shared_ptr<Listener> listener;
    std::unique_ptr<Queue> queue;
    SubscriberMode mode = SubscriberMode::PUSH;
    std::optional<std::jthread> thread;
  };

#if FLOX_CPU_AFFINITY_ENABLED
  std::unique_ptr<performance::CpuAffinity> _cpuAffinity;
  std::optional<performance::CoreAssignment> _coreAssignment;
  std::optional<AffinityConfig> _affinityConfig;
#endif
  std::unordered_map<SubscriberId, Entry> _subs;
  std::mutex _mutex;
  std::atomic<bool> _running{false};
  std::atomic<size_t> _active{0};
  std::condition_variable _cv;
  std::mutex _readyMutex;
  std::atomic<uint64_t> _tickCounter{0};
  bool _drainOnStop = false;
};

}  // namespace flox
