/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/log/log.h"
#include "flox/util/base/time.h"

#define NO_COUT 1

#include "demo/demo_builder.h"
#include "demo/latency_collector.h"

#include <chrono>
#include <thread>

demo::LatencyCollector collector;

int main()
{
  flox::init_timebase_mapping();

  demo::EngineConfig cfg{};
  demo::DemoBuilder builder(cfg);
  auto engine = builder.build();

#if NO_COUT
  FLOX_LOG_OFF();
#endif

  engine->start();

  std::this_thread::sleep_for(std::chrono::seconds(30));

  engine->stop();

#if NO_COUT
  FLOX_LOG_ON();
#endif

  FLOX_LOG("demo finished");

  collector.report();
}
