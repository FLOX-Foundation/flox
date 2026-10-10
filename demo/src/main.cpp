/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/log/log.h"
#include "lrvx/util/base/time.h"

#define NO_COUT 1

#include "demo/demo_builder.h"
#include "demo/latency_collector.h"

#include <chrono>
#include <thread>

demo::LatencyCollector collector;

int main()
{
  lrvx::init_timebase_mapping();

  demo::EngineConfig cfg{};
  demo::DemoBuilder builder(cfg);
  auto engine = builder.build();

#if NO_COUT
  LRVX_LOG_OFF();
#endif

  engine->start();

  std::this_thread::sleep_for(std::chrono::seconds(30));

  engine->stop();

#if NO_COUT
  LRVX_LOG_ON();
#endif

  LRVX_LOG("demo finished");

  collector.report();
}
