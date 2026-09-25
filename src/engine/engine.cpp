/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/engine.h"

#include "flox/log/log.h"
#include "flox/util/base/time.h"
#include "flox/util/memory/counting_resource.h"
#include "flox/util/memory/large_arena.h"
#include "flox/util/performance/memory_profile.h"

#include <utility>

namespace flox
{

Engine::Engine(const EngineConfig& config, std::vector<std::unique_ptr<ISubsystem>> subsystems,
               std::vector<std::shared_ptr<IExchangeConnector>> connectors)
    : _config(config), _subsystems(std::move(subsystems)), _connectors(std::move(connectors))
{
}

void Engine::start()
{
  // First, before anything the engine owns runs. Every connector, aggregator
  // and bar policy that turns a venue's wall-clock stamp into a FloxClock
  // TimePoint reads the process-global offset this establishes, and a
  // component started before it would convert its first messages through a
  // zero offset and the rest through the anchored one. Idempotent, so a
  // second engine in the same process keeps the mapping the first one set.
  init_timebase_mapping();

  auto memReport = performance::applyMemoryProfile(
      performance::memoryProfileFromString(_config.memoryProfile));
  memReport.hugeArenaBytes = memory::LargeArena::totalBytesAll();
  memReport.hugeArenaMode = memory::LargeArena::aggregateMode();
  memReport.poolHeapAllocations = memory::CountingResource::totalAllocations();
  memReport.poolHeapBytes = memory::CountingResource::totalBytes();
  FLOX_LOG_INFO(memReport.toString());
  if (memReport.poolHeapAllocations > 0)
  {
    FLOX_LOG_WARN("pool arenas fell back to the heap "
                  << memReport.poolHeapAllocations << " time(s) ("
                  << memReport.poolHeapBytes
                  << "B): a pool's inline buffer is too small for its events. Size it at "
                     "startup with Pool::prewarm() so this does not happen mid-session.");
  }
  if (memReport.mlockRequested && !memReport.mlockApplied)
  {
    FLOX_LOG_WARN("memory profile 'colo' requested but mlockall failed ("
                  << memReport.mlockError
                  << "); hot pages remain evictable. Check CAP_IPC_LOCK / ulimit -l.");
  }

  for (auto& subsystem : _subsystems)
  {
    subsystem->start();
  }

  for (auto& connector : _connectors)
  {
    connector->start();
  }
}

void Engine::stop()
{
  // Drain and stop connectors first (wait for in-flight orders)
  for (auto& connector : _connectors)
  {
    connector->drain(_config.drainTimeoutMs);
  }

  for (auto& connector : _connectors)
  {
    connector->stop();
  }

  // Stop subsystems in reverse order. Walking backwards rather than reversing
  // in place: the vector is the engine's start order, and reordering it here
  // leaves a second start() bringing the strategy up before the bus it
  // publishes into and a second stop() shutting that bus down first.
  for (auto it = _subsystems.rbegin(); it != _subsystems.rend(); ++it)
  {
    (*it)->stop();
  }
}

}  // namespace flox