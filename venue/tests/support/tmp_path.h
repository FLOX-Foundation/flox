/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
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

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace lrvx::venue::test
{

// "<temp dir>/lrvx_test_<stem>_<pid><ext>". Call it once per path and keep the
// result: a test that forks must hand the child the parent's string, not
// recompute it on the other side of the fork.
inline std::string tmpPath(const std::string& stem, const std::string& ext = "")
{
#if defined(_WIN32)
  const int pid = ::_getpid();
#else
  const int pid = static_cast<int>(::getpid());
#endif
  // The temporary directory is asked for rather than spelled: "/tmp" does not
  // exist on every platform this builds on, and a path that does not exist
  // fails at run time rather than at compile time.
  const auto dir = std::filesystem::temp_directory_path();
  return (dir / ("lrvx_test_" + stem + "_" + std::to_string(pid) + ext)).string();
}

}  // namespace lrvx::venue::test
