/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Process-qualified scratch paths for the venue tests.
 *
 * A journal opens in truncate mode, so two processes sharing one path erase
 * and then read each other's file. That happens whenever this binary runs
 * twice at once -- a manual rerun alongside `ctest -j`, gtest sharding, two CI
 * jobs on one runner -- and it does not present as a path collision. It
 * presents as replay returning the wrong records, which is a long way from the
 * cause. Every scratch path a test writes carries the pid.
 */
#pragma once

#include <unistd.h>

#include <string>

namespace flox::venue::test
{

// "/tmp/flox_test_<stem>_<pid><ext>". Call it once per path and keep the
// result: a test that forks must hand the child the parent's string, not
// recompute it on the other side of the fork.
inline std::string tmpPath(const std::string& stem, const std::string& ext = "")
{
  return "/tmp/flox_test_" + stem + "_" + std::to_string(::getpid()) + ext;
}

}  // namespace flox::venue::test
