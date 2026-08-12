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
#include "flox/util/memory/counting_resource.h"
#include "flox/util/memory/large_arena.h"
#include "flox/util/performance/memory_profile.h"

#include <algorithm>
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

  // Stop subsystems in reverse order
  std::reverse(_subsystems.begin(), _subsystems.end());
  for (auto& subsystem : _subsystems)
  {
    subsystem->stop();
  }
}

}  // namespace flox