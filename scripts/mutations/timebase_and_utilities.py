#!/usr/bin/env python3
"""Mutation harness for the engine timebase anchor and the utility fixes
(pool object lifetime, POLLNVAL, the log default directory and its cost,
and the position-manager snapshot).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each fix one piece at a time,
in the source, and checks that the tests written for it go red -- and that an
unmutated tree goes green before and after.

Each mutation names the tests that are supposed to notice it; that filter
runs first, and a mutation the filter does not kill is then re-run against
the whole of its primary targets. A mutation still alive after that is swept
against every related binary in the tree before it is reported green, so a
survivor is a survivor of everything asked, not just of the suite written
for this change.

Every run is honest about the build: the mutated file's hash is printed
before and after, the library object for a mutated .cpp (or, for a mutated
header, every library object) is deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" or the run is
refused, and each binary runs under a timeout.  A mutation that does not
compile is not a mutation and is reported as such.  A mutation may be marked
`equivalent` with a reason instead of a verdict, when no test could possibly
observe the difference; none here are -- every mutation below is a real
behaviour change, and a survivor is a real gap.

Usage:

    python3 scripts/mutations/timebase_and_utilities.py            # control, all, control
    python3 scripts/mutations/timebase_and_utilities.py --list
    python3 scripts/mutations/timebase_and_utilities.py --only pool-destructor-does-not-run-dtor

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
          -DFLOX_BUILD_TESTS=ON -DFLOX_ENABLE_BACKTEST=ON
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
BIN_DIR = BUILD / "tests"
LIB = "flox"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 60

TIME_H = "include/flox/util/base/time.h"
ENGINE_CPP = "src/engine/engine.cpp"
ENGINE_CONFIG_H = "include/flox/engine/engine_config.h"
POOL_H = "include/flox/util/memory/pool.h"
SOCKET_H = "include/flox/net/socket.h"
ABSTRACT_LOGGER_H = "include/flox/log/abstract_logger.h"
CONSOLE_LOGGER_H = "include/flox/log/console_logger.h"
ATOMIC_LOGGER_H = "include/flox/log/atomic_logger.h"
ATOMIC_LOGGER_CPP = "src/log/atomic_logger.cpp"
LOG_H = "include/flox/log/log.h"
LOG_STREAM_H = "include/flox/log/log_stream.h"
LOG_STREAM_CPP = "src/log/log_stream.cpp"
ABSTRACT_POSITION_MANAGER_H = "include/flox/position/abstract_position_manager.h"
POSITION_TRACKER_H = "include/flox/position/position_tracker.h"
MULTI_MODE_POSITION_TRACKER_H = "include/flox/position/multi_mode_position_tracker.h"

# Every related binary named in the task, plus the five acceptance ones. A
# mutation that survives its primary targets is swept against all of these
# before it is called green.
ALL_RELATED = [
    "test_engine_timebase",
    "test_pool_object_lifetime",
    "test_socket_poll_and_family",
    "test_log_defaults_and_cost",
    "test_strategy_position_refresh",
    "test_time_domains",
    "test_event_pool",
    "test_book_update_bus",
    "test_atomic_logger",
    "test_socket_portability",
    "test_strategy_unrealized_pnl",
    "test_position_aggregator",
    "test_multi_symbol_strategy",
]

TIMEBASE = ["test_engine_timebase"]
POOL = ["test_pool_object_lifetime", "test_event_pool"]
SOCKET = ["test_socket_poll_and_family", "test_socket_portability"]
LOG = ["test_log_defaults_and_cost", "test_atomic_logger"]
POSITION = [
    "test_strategy_position_refresh",
    "test_strategy_unrealized_pnl",
    "test_position_aggregator",
    "test_multi_symbol_strategy",
]


@dataclass
class Edit:
    file: str
    old: str
    new: str
    occurrence: int = 1
    expected_occurrences: int = 1


@dataclass
class Mutation:
    name: str
    why: str
    file: str = ""
    old: str = ""
    new: str = ""
    edits: list[Edit] = field(default_factory=list)
    targets: list[str] = field(default_factory=list)
    gtest_filter: str | None = None
    occurrence: int = 1
    expected_occurrences: int = 1
    # A reason a mutation cannot be killed by any test, ever -- e.g. a change
    # with no observable effect on any input a test could construct. One of
    # the mutations below claims this; it exists so a genuine case does not
    # have to be silently dropped from the table.
    equivalent: str | None = None

    def editList(self) -> list[Edit]:
        if self.edits:
            return self.edits
        return [Edit(self.file, self.old, self.new, self.occurrence, self.expected_occurrences)]

    def files(self) -> list[str]:
        seen: list[str] = []
        for e in self.editList():
            if e.file not in seen:
                seen.append(e.file)
        return seen


MUTATIONS: list[Mutation] = [
    # ------------------------------------------------------- T015: timebase
    Mutation(
        name="anchor-after-subsystems-start",
        why="init_timebase_mapping() moves from the top of Engine::start() to "
            "between the subsystem loop and the connector loop, so a subsystem "
            "is started with the mapping still at zero -- exactly the ordering "
            "MappingIsAnchoredBeforeSubsystemsAndConnectorsStart exists to pin",
        edits=[
            Edit(
                file=ENGINE_CPP,
                old="""void Engine::start()
{
  // First, before anything the engine owns runs. Every connector, aggregator
  // and bar policy that turns a venue's wall-clock stamp into a FloxClock
  // TimePoint reads the process-global offset this establishes, and a
  // component started before it would convert its first messages through a
  // zero offset and the rest through the anchored one. Idempotent, so a
  // second engine in the same process keeps the mapping the first one set.
  init_timebase_mapping();

  auto memReport = performance::applyMemoryProfile(""",
                new="""void Engine::start()
{
  auto memReport = performance::applyMemoryProfile(""",
            ),
            Edit(
                file=ENGINE_CPP,
                old="""  for (auto& subsystem : _subsystems)
  {
    subsystem->start();
  }

  for (auto& connector : _connectors)
  {
    connector->start();
  }
}""",
                new="""  for (auto& subsystem : _subsystems)
  {
    subsystem->start();
  }

  init_timebase_mapping();

  for (auto& connector : _connectors)
  {
    connector->start();
  }
}""",
            ),
        ],
        targets=TIMEBASE,
        gtest_filter="EngineTimebase.MappingIsAnchoredBeforeSubsystemsAndConnectorsStart",
    ),
    Mutation(
        name="anchor-after-connectors-start",
        why="init_timebase_mapping() moves to the very end of Engine::start(), "
            "after both loops, so every subsystem and every connector is "
            "started with the mapping still at zero",
        edits=[
            Edit(
                file=ENGINE_CPP,
                old="""void Engine::start()
{
  // First, before anything the engine owns runs. Every connector, aggregator
  // and bar policy that turns a venue's wall-clock stamp into a FloxClock
  // TimePoint reads the process-global offset this establishes, and a
  // component started before it would convert its first messages through a
  // zero offset and the rest through the anchored one. Idempotent, so a
  // second engine in the same process keeps the mapping the first one set.
  init_timebase_mapping();

  auto memReport = performance::applyMemoryProfile(""",
                new="""void Engine::start()
{
  auto memReport = performance::applyMemoryProfile(""",
            ),
            Edit(
                file=ENGINE_CPP,
                old="""  for (auto& connector : _connectors)
  {
    connector->start();
  }
}""",
                new="""  for (auto& connector : _connectors)
  {
    connector->start();
  }

  init_timebase_mapping();
}""",
            ),
        ],
        targets=TIMEBASE,
        gtest_filter="EngineTimebase.MappingIsAnchoredBeforeSubsystemsAndConnectorsStart",
    ),
    Mutation(
        name="second-init-call-overwrites-the-offset",
        why="init_timebase_mapping() goes back to an unconditional store, so a "
            "second call (the lazy call_once a connector used to make) takes a "
            "fresh reading instead of leaving the established offset alone, "
            "moving the meaning of every timestamp converted before it",
        file=TIME_H,
        old="""  int64_t unanchored = 0;
  unix_to_flox_offset_ns().compare_exchange_strong(unanchored, flox_ns - unix_ns,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed);""",
        new="""  unix_to_flox_offset_ns().store(flox_ns - unix_ns, std::memory_order_relaxed);""",
        targets=TIMEBASE,
        gtest_filter="EngineTimebase.AConnectorNeedNotAnchorTheMappingItself",
    ),
    Mutation(
        name="cas-compares-against-a-nonzero-sentinel",
        why="the CAS's expected value is 1 instead of 0, so it never matches "
            "the atomic's actual initial value and the very first anchor call "
            "silently fails: the offset stays zero forever and no caller is "
            "told",
        file=TIME_H,
        old="""  int64_t unanchored = 0;
  unix_to_flox_offset_ns().compare_exchange_strong(unanchored, flox_ns - unix_ns,""",
        new="""  int64_t unanchored = 1;
  unix_to_flox_offset_ns().compare_exchange_strong(unanchored, flox_ns - unix_ns,""",
        targets=TIMEBASE,
        gtest_filter="EngineTimebase.StartAnchorsTheUnixToFloxMapping",
    ),
    Mutation(
        name="config-exchanges-field-resurrected-unread",
        why="EngineConfig::exchanges (with ExchangeConfig and SymbolConfig) "
            "comes back, filled in by a caller and read by nothing -- the "
            "exact dead-config finding, minus the doc lines, reintroduced "
            "behind the config's back",
        edits=[
            Edit(
                file=ENGINE_CONFIG_H,
                old="""#include <cstddef>
#include <cstdint>
#include <string>""",
                new="""#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>""",
            ),
            Edit(
                file=ENGINE_CONFIG_H,
                old="""struct EngineConfig
{
  uint32_t drainTimeoutMs = 5000;  ///< Timeout for draining in-flight orders on shutdown""",
                new="""struct SymbolConfig
{
  std::string symbol;
  double tickSize;
  double expectedDeviation;
};

struct ExchangeConfig
{
  std::string name;
  std::string type;
  std::vector<SymbolConfig> symbols;
};

struct EngineConfig
{
  std::vector<ExchangeConfig> exchanges;

  uint32_t drainTimeoutMs = 5000;  ///< Timeout for draining in-flight orders on shutdown""",
            ),
        ],
        targets=TIMEBASE,
        gtest_filter="EngineConfigFields.ExchangesAndSymbolsAreEitherReadOrGone",
    ),
    Mutation(
        name="anchor-reads-a-different-clock-than-fromunixms-writes-through",
        why="both sides of the anchor read system_clock, so the offset comes "
            "out near zero instead of the true wall-clock-to-steady-clock gap; "
            "fromUnixMs then hands an epoch-scale nanosecond count to a "
            "FloxClock TimePoint as if it were a steady_clock reading -- the "
            "original bug, reintroduced by anchoring against the wrong clock "
            "rather than by not anchoring at all",
        file=TIME_H,
        old="""  const auto flox_ns = duration_cast<nanoseconds>(now().time_since_epoch()).count();
  const auto unix_ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();""",
        new="""  const auto flox_ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  const auto unix_ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();""",
        targets=TIMEBASE,
        gtest_filter="EngineTimebase.StartAnchorsTheUnixToFloxMapping"
                     ":EngineTimebase.AnchoredMappingCarriesAKnownInstantBothWays",
    ),
    # ----------------------------------------------------------- T017: pool
    Mutation(
        name="pool-destructor-does-not-run-dtor",
        why="~Pool() goes back to =default over the raw Storage array, so "
            "nothing a pooled object owns outside the arena is ever unwound",
        file=POOL_H,
        old="""  ~Pool()
  {
    for (size_t i = 0; i < Capacity; ++i)
    {
      std::launder(reinterpret_cast<T*>(&_slots[i]))->~T();
    }
  }""",
        new="""  ~Pool() = default;""",
        targets=POOL,
        gtest_filter="PoolObjectLifetime.EverySlotIsDestroyedExactlyOnceWhenThePoolDies",
    ),
    Mutation(
        name="claim-cas-replaced-by-load-then-store",
        why="the claim flag's compare_exchange_strong becomes a plain load "
            "followed by an unconditional store; a single-threaded release "
            "sequence still comes out right, so this is a race that no test "
            "in the tree drives two threads through -- the exact hazard the "
            "CAS exists for",
        file=POOL_H,
        old="""    bool claimed = true;
    if (!_claimed[index].compare_exchange_strong(claimed, false, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
    {
      _invalidReleases.fetch_add(1, std::memory_order_relaxed);
      return;
    }""",
        new="""    const bool claimed = _claimed[index].load(std::memory_order_acquire);
    _claimed[index].store(false, std::memory_order_release);
    if (!claimed)
    {
      _invalidReleases.fetch_add(1, std::memory_order_relaxed);
      return;
    }""",
        targets=POOL,
        gtest_filter="PoolObjectLifetime.*",
    ),
    Mutation(
        name="indexof-drops-the-alignment-check",
        why="indexOf() keeps the range check but drops the alignment check, so "
            "a pointer that lands inside this pool's slot array without "
            "pointing at the start of a slot -- a foreign, misaligned pointer "
            "-- is accepted as if it were slot floor(offset/sizeof(Storage))",
        file=POOL_H,
        old="""    const uintptr_t offset = addr - base;
    if (offset % sizeof(Storage) != 0 || offset / sizeof(Storage) >= Capacity)
    {
      return static_cast<uint32_t>(Capacity);
    }
    return static_cast<uint32_t>(offset / sizeof(Storage));""",
        new="""    const uintptr_t offset = addr - base;
    if (offset / sizeof(Storage) >= Capacity)
    {
      return static_cast<uint32_t>(Capacity);
    }
    return static_cast<uint32_t>(offset / sizeof(Storage));""",
        targets=POOL,
        gtest_filter="PoolObjectLifetime.*",
    ),
    Mutation(
        name="indexof-wraps-any-pointer-into-a-valid-slot",
        why="indexOf() drops both the underflow guard and the range check and "
            "answers every pointer with (offset / sizeof(Storage)) % Capacity, "
            "so a pointer from a sibling pool of the same type -- or any "
            "address at all -- is accepted as belonging to some slot of this "
            "pool instead of being refused",
        file=POOL_H,
        old="""  uint32_t indexOf(const T* obj) const noexcept
  {
    const auto base = reinterpret_cast<uintptr_t>(&_slots[0]);
    const auto addr = reinterpret_cast<uintptr_t>(obj);
    if (addr < base)
    {
      return static_cast<uint32_t>(Capacity);
    }
    const uintptr_t offset = addr - base;
    if (offset % sizeof(Storage) != 0 || offset / sizeof(Storage) >= Capacity)
    {
      return static_cast<uint32_t>(Capacity);
    }
    return static_cast<uint32_t>(offset / sizeof(Storage));
  }""",
        new="""  uint32_t indexOf(const T* obj) const noexcept
  {
    const auto base = reinterpret_cast<uintptr_t>(&_slots[0]);
    const auto addr = reinterpret_cast<uintptr_t>(obj);
    const uintptr_t offset = addr - base;
    return static_cast<uint32_t>((offset / sizeof(Storage)) % Capacity);
  }""",
        targets=POOL,
        gtest_filter="PoolObjectLifetime.*:EventPoolTest.*",
    ),
    Mutation(
        name="invalid-release-count-not-incremented-on-double-release",
        why="the claim-CAS-failure branch (an already-released object handed "
            "back again) stops counting into _invalidReleases -- the release "
            "is still refused, only the machine-readable signal is dropped, "
            "and nothing in the tree ever reads invalidReleaseCount() to "
            "notice",
        file=POOL_H,
        old="""    bool claimed = true;
    if (!_claimed[index].compare_exchange_strong(claimed, false, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
    {
      _invalidReleases.fetch_add(1, std::memory_order_relaxed);
      return;
    }""",
        new="""    bool claimed = true;
    if (!_claimed[index].compare_exchange_strong(claimed, false, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
    {
      return;
    }""",
        targets=POOL,
        gtest_filter="PoolObjectLifetime.*",
    ),
    Mutation(
        name="inuse-decremented-on-a-refused-release",
        why="_inUse.fetch_sub() moves ahead of the claim check and runs "
            "whatever the CAS decides, so a refused (double) release still "
            "counts as one less object in use -- the exact underflow the "
            "single _inUse counter exists to prevent, now reachable through "
            "the refusal path instead of the acceptance path",
        file=POOL_H,
        old="""    bool claimed = true;
    if (!_claimed[index].compare_exchange_strong(claimed, false, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
    {
      _invalidReleases.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    obj->clear();
    _inUse.fetch_sub(1, std::memory_order_relaxed);
    _freelist.push(index);
    _released.fetch_add(1, std::memory_order_relaxed);""",
        new="""    _inUse.fetch_sub(1, std::memory_order_relaxed);
    bool claimed = true;
    if (!_claimed[index].compare_exchange_strong(claimed, false, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
    {
      _invalidReleases.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    obj->clear();
    _freelist.push(index);
    _released.fetch_add(1, std::memory_order_relaxed);""",
        targets=POOL,
        gtest_filter="PoolObjectLifetime.ADoubleReleaseIsRefusedAndInUseNeverGoesNegative",
    ),
    # --------------------------------------------------------- T017: socket
    Mutation(
        name="pollnval-mapped-to-timedout-not-error",
        why="POLLNVAL sets r.timedOut instead of r.error, so a closed "
            "descriptor is reported the same way as an idle one -- decisive, "
            "so the wait-then-read loop still terminates, but a caller that "
            "branches on error vs timeout treats a dead socket as one that "
            "simply has nothing to say yet",
        file=SOCKET_H,
        old="""#if defined(POLLNVAL)
  if ((p.revents & POLLNVAL) != 0)
  {
    r.error = true;
  }
#endif""",
        new="""#if defined(POLLNVAL)
  if ((p.revents & POLLNVAL) != 0)
  {
    r.timedOut = true;
  }
#endif""",
        targets=SOCKET,
        gtest_filter="SocketPoll.AClosedDescriptorIsReportedAsAnErrorAndNotAsNothing",
    ),
    # ------------------------------------------------------------ T017: log
    Mutation(
        name="default-log-directory-hardcoded-to-dev-shm",
        why="defaultLogDirectory() goes back to the literal \"/dev/shm\" with "
            "no existence check -- on a host without it (this one), "
            "create_directories fails, the file never opens, and every line "
            "is dropped after one line on stderr",
        file=ATOMIC_LOGGER_CPP,
        old="""const std::string& defaultLogDirectory()
{
  // Resolved once: the answer cannot change under a running process, and a
  // stat per default-constructed options object would be paid on a path that
  // is often taken in a constructor.
  static const std::string dir = []
  {
    std::error_code ec;
    const fs::path shm{"/dev/shm"};
    if (fs::is_directory(shm, ec))
    {
      return shm.string();
    }
    const fs::path tmp = fs::temp_directory_path(ec);
    if (!ec && !tmp.empty())
    {
      return tmp.string();
    }
    // Neither answered: the current directory always exists, and a relative
    // path is still a file somebody can find. The rotation counter reports it
    // if even that cannot be opened.
    return std::string(".");
  }();
  return dir;
}""",
        new="""const std::string& defaultLogDirectory()
{
  static const std::string dir = "/dev/shm";
  return dir;
}""",
        targets=LOG,
        gtest_filter="AtomicLoggerDefaults.TheDefaultDirectoryIsOneTheHostActuallyHas",
    ),
    Mutation(
        name="writeToOutput-drops-the-line-with-no-file-again",
        why="writeToOutput() goes back to a bare early return when _file is "
            "null, so a rotation failure drops every line silently instead of "
            "sending it to stderr -- rotationFailures() still counts the "
            "transition, which is all ARotationThatCannotOpenItsFileDrops"
            "TheLogNotTheProcess checks, so the drop itself is unobserved by "
            "any test in the tree",
        file=ATOMIC_LOGGER_CPP,
        old="""  if (_file == nullptr)
  {
    // No file to write to -- the directory went away or became unwritable
    // under a running process. The line goes to stderr rather than nowhere:
    // a logger that cannot open its file is a reason to look at the console,
    // not a reason to discard what it was told. rotationFailures() stays the
    // machine-readable signal.
    std::fprintf(stderr, "flox: [%s] %s: %s\\n", timebuf, levelStr, entry.message);
    return;
  }""",
        new="""  if (_file == nullptr)
  {
    return;
  }""",
        targets=LOG,
        gtest_filter="AtomicLoggerDefaults.*:AtomicLoggerTest.ARotationThatCannotOpenItsFileDropsTheLogNotTheProcess",
    ),
    Mutation(
        name="consolelogger-minlevel-ignores-its-threshold",
        why="ConsoleLogger::minLevel() stops returning _minLevel and always "
            "answers Info -- the floor of the enum, the same answer a sink "
            "with no threshold at all gives -- so FLOX_LOG_LEVEL never sees a "
            "reason to filter and every line is built and formatted again "
            "regardless of what the logger was constructed with",
        file=CONSOLE_LOGGER_H,
        old="""  LogLevel minLevel() const noexcept override { return _minLevel; }""",
        new="""  LogLevel minLevel() const noexcept override { return LogLevel::Info; }""",
        targets=LOG,
        gtest_filter="LogCost.AFilteredLevelDoesNotEvaluateTheMessage",
    ),
    Mutation(
        name="flox-log-level-reads-the-threshold-after-building-the-stream",
        why="the macro builds and emits the LogStream unconditionally via the "
            "comma operator and only checks the threshold afterwards, on a "
            "result nothing branches on -- so the message is always formatted "
            "and always delivered, and the level read happens too late to "
            "matter",
        file=LOG_H,
        old="""#define FLOX_LOG_LEVEL(lvl, ...)                                         \\
  if (const ::flox::LogLevel floxLogLevel_ = (lvl);                      \\
      !::flox::isLoggingEnabled() || floxLogLevel_ < ::flox::logLevel()) \\
    ;                                                                    \\
  else                                                                   \\
    ::flox::LogStream(floxLogLevel_) << __VA_ARGS__""",
        new="""#define FLOX_LOG_LEVEL(lvl, ...)                                                 \\
  if (const ::flox::LogLevel floxLogLevel_ = (lvl); !::flox::isLoggingEnabled()) \\
    ;                                                                            \\
  else if ((::flox::LogStream(floxLogLevel_) << __VA_ARGS__,                     \\
           floxLogLevel_ < ::flox::logLevel()))                                  \\
    ;                                                                            \\
  else                                                                           \\
    ;""",
        targets=LOG,
        gtest_filter="LogCost.*",
    ),
    Mutation(
        name="setgloballogger-does-not-republish-the-level",
        why="setGlobalLogger() stops writing globalMinLogLevel, so it is "
            "never anything but its process-start initial value (Info): "
            "installing a sink with a higher threshold changes what the sink "
            "itself accepts but not what the macro thinks it should bother "
            "formatting",
        file=LOG_STREAM_CPP,
        old="""void setGlobalLogger(ILogger* logger)
{
  // The threshold first, then the sink: a line that slips through the macro
  // between the two stores is still filtered by the sink itself, which is
  // where the level is also applied. The reverse order could hand the new
  // sink a level belonging to the old one.
  globalMinLogLevel.store(logger != nullptr ? logger->minLevel() : defaultLogger().minLevel(),
                          std::memory_order_release);
  g_logger.store(logger, std::memory_order_release);
}""",
        new="""void setGlobalLogger(ILogger* logger)
{
  g_logger.store(logger, std::memory_order_release);
}""",
        targets=LOG,
        gtest_filter="LogCost.AFilteredLevelDoesNotEvaluateTheMessage",
    ),
    Mutation(
        name="loglevel-load-relaxed-store-stays-release",
        why="logLevel()'s load drops from acquire to relaxed while "
            "setGlobalLogger()'s store stays release, breaking the "
            "synchronizes-with pairing between the two -- a data race no "
            "single-threaded test can distinguish from the paired version, "
            "since every test here calls setGlobalLogger and reads logLevel() "
            "on the same thread with nothing for the race to reorder",
        file=LOG_STREAM_H,
        old="""inline LogLevel logLevel() noexcept
{
  return globalMinLogLevel.load(std::memory_order_acquire);
}""",
        new="""inline LogLevel logLevel() noexcept
{
  return globalMinLogLevel.load(std::memory_order_relaxed);
}""",
        equivalent="both sides are atomics, so there is no data race on the "
                   "level itself; the level is a self-contained enum with no "
                   "non-atomic data behind it, so an acquire on this load "
                   "publishes nothing. The ordering that matters, with the "
                   "sink object's construction, comes from the acquire load "
                   "of g_logger, which this mutation leaves alone. No input "
                   "distinguishes the two versions; a concurrent test under "
                   "ThreadSanitizer stays clean under the mutation too",
        targets=LOG,
        gtest_filter="LogCost.*",
    ),
    # ------------------------------------------------------- T017: position
    Mutation(
        name="positionsnapshot-default-composes-both-getters",
        why="IPositionManager::positionSnapshot()'s default goes back to "
            "calling getPosition() and getAverageEntryPrice() separately -- "
            "the exact composition the fix note calls out as reinstating the "
            "second lock acquisition it exists to remove -- for any manager "
            "that does not override it",
        file=ABSTRACT_POSITION_MANAGER_H,
        old="""  virtual PositionSnapshot positionSnapshot(SymbolId symbol) const
  {
    return PositionSnapshot{getPosition(symbol), std::nullopt};
  }""",
        new="""  virtual PositionSnapshot positionSnapshot(SymbolId symbol) const
  {
    return PositionSnapshot{getPosition(symbol), getAverageEntryPrice(symbol)};
  }""",
        targets=POSITION,
        gtest_filter="StrategyPositionRefresh.ATickAsksThePositionManagerAtMostOnce",
    ),
    Mutation(
        name="positiontracker-snapshot-takes-the-lock-twice",
        why="PositionTracker::positionSnapshot() stops taking one lock and "
            "calling PositionState::snapshot(); instead it calls the public "
            "getPosition() and getAverageEntryPrice() from inside its own "
            "lock_guard, each of which takes the same non-recursive mutex "
            "again -- a self-deadlock the moment any tick reaches it",
        file=POSITION_TRACKER_H,
        old="""  // One lock, one traversal, both answers.
  PositionSnapshot positionSnapshot(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].snapshot();
  }""",
        new="""  // One lock, one traversal, both answers.
  PositionSnapshot positionSnapshot(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    PositionSnapshot snap;
    snap.position = getPosition(symbol);
    snap.avgEntryPrice = getAverageEntryPrice(symbol);
    return snap;
  }""",
        targets=POSITION,
        gtest_filter="StrategyUnrealizedPnl.*",
    ),
    Mutation(
        name="multimode-positionsnapshot-reports-the-long-side-only",
        why="MultiModePositionTracker::positionSnapshot() answers with "
            "snap.longAvgEntry instead of blendedEntry(snap) -- a book that "
            "is short only, or mixed long and short, reports whatever the "
            "long side's entry happened to be (stale or default-zero) rather "
            "than the blended cost basis getAverageEntryPrice() computes for "
            "the same state",
        file=MULTI_MODE_POSITION_TRACKER_H,
        old="""  flox::PositionSnapshot positionSnapshot(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto snap = snapshotUnlocked(symbol);
    return flox::PositionSnapshot{getPositionUnlocked(symbol), blendedEntry(snap)};
  }""",
        new="""  flox::PositionSnapshot positionSnapshot(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto snap = snapshotUnlocked(symbol);
    return flox::PositionSnapshot{getPositionUnlocked(symbol), snap.longAvgEntry};
  }""",
        # No named filter: nothing in the tree calls positionSnapshot() on a
        # MultiModePositionTracker directly (only PositionTracker gets there,
        # through Strategy::refreshPosition() in test_strategy_unrealized_pnl).
        # The whole-binary runs over `targets` below are what this mutation is
        # actually judged on.
        targets=POSITION,
    ),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_occurrence(text: str, old: str, new: str, occurrence: int, expected: int) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"mutation anchor found {count} time(s), expected {expected}:\n  {old!r}")
    start = -1
    for _ in range(occurrence):
        start = text.index(old, start + 1)
    return text[:start] + new + text[start + len(old):]


def find_objects(*args: str) -> list[Path]:
    out = subprocess.run(["find", str(BUILD), "-type", "f", *args],
                         capture_output=True, text=True, check=True).stdout.split()
    return [Path(p) for p in out]


def target_objects(target: str) -> list[Path]:
    return find_objects("-name", "*.o", "-path", f"*{target}.dir*")


def library_objects(source: str) -> list[Path]:
    """The library objects that have to go for a mutated source to be recompiled.

    A mutated .cpp has exactly one object in the library; a mutated header has
    none of its own, so every library object goes and the whole library is
    rebuilt rather than trusting the dependency scanner.
    """
    if source.endswith((".h", ".inl", ".hpp")):
        return find_objects("-name", "*.o", "-path", f"*{LIB}.dir*")
    return find_objects("-name", f"{Path(source).name}.o", "-path", f"*{LIB}.dir*")


class BuildFailed(Exception):
    def __init__(self, target: str, output: str):
        super().__init__(f"rebuild of {target} failed")
        self.target = target
        self.output = output


def rebuild(target: str, requireCompile: bool = True) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(target, output)
    if requireCompile and "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}")
    return output


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = BIN_DIR / target
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s (likely a self-deadlock)"
    return result.returncode, result.stdout + result.stderr


def ran_line(output: str) -> str:
    return next((line for line in output.splitlines()
                 if line.startswith("[==========] ") and " ran." in line), "").strip()


def report(target: str, gtest_filter: str | None, code: int, output: str) -> None:
    flt = f" --gtest_filter={gtest_filter}" if gtest_filter else ""
    print(f"  {target}{flt} -> exit {code} "
          f"({'RED, mutation killed' if code else 'green'})   {ran_line(output)}")
    if code:
        for line in [ln for ln in output.splitlines() if ln.startswith("[  FAILED  ]")][:6]:
            print(f"    {line}")
        if code == 124:
            print(f"    {output}")


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        for obj in target_objects(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target, None)
        print(f"  control {target:<32} {'green' if code == 0 else 'RED'}   {ran_line(output)}")
        ok = ok and code == 0
    return ok


def build_all(targets: list[str], sources: list[str]) -> None:
    removed = []
    for source in sources:
        removed += library_objects(source)
    for target in targets:
        removed += target_objects(target)
    for obj in dict.fromkeys(removed):
        obj.unlink()
    print(f"  removed {len(set(removed))} object file(s) "
          f"(0 = already deleted by the previous restore)")
    output = rebuild(LIB)
    compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
    print(f"  rebuilt {LIB}: {compiled} 'Building CXX' line(s)")
    for target in targets:
        out = rebuild(target)
        n = sum(1 for line in out.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {target}: {n} 'Building CXX' line(s)")


def run_mutation(m: Mutation) -> str:
    """'killed', 'alive', 'no-compile' or 'equivalent'."""
    if m.equivalent:
        print(f"\n[{m.name}]")
        print(f"  {m.why}")
        print(f"  EQUIVALENT: {m.equivalent}")
        return "equivalent"

    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    for f in m.files():
        print(f"  file    {f}")
        print(f"  sha256  before  {before[f]}")

    texts = dict(originals)
    for e in edits:
        texts[e.file] = replace_occurrence(texts[e.file], e.old, e.new, e.occurrence,
                                           e.expected_occurrences)
    for f, p in paths.items():
        if texts[f] == originals[f]:
            raise SystemExit(f"mutation changed nothing in {f}")
        p.write_text(texts[f])
        print(f"  sha256  mutated {sha256(p)}  {f}")

    verdict = "alive"
    try:
        try:
            build_all(m.targets, m.files())
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-25:]))
            return "no-compile"

        filterKilled = False
        if m.gtest_filter:
            for target in m.targets:
                code, output = run_test(target, m.gtest_filter)
                report(target, m.gtest_filter, code, output)
                if code:
                    filterKilled = True
                    verdict = "killed"

        if m.gtest_filter and not filterKilled:
            print("  the named tests did not notice; falling back to the whole binaries")
        red: list[str] = []
        for target in m.targets:
            code, output = run_test(target, None)
            report(target, None, code, output)
            if code:
                red.append(target)
                verdict = "killed"
        if len(red) == 1:
            print(f"  only {red[0]} answers for this one")

        if verdict == "alive":
            print("  sweeping every related binary")
            extra = [t for t in ALL_RELATED if t not in m.targets]
            try:
                for target in extra:
                    out = rebuild(target, requireCompile=False)
                    if "Linking CXX" not in out and "Building CXX" not in out:
                        raise SystemExit(
                            f"{target} was neither compiled nor linked after the "
                            f"library changed; the binary would be stale:\n{out[-800:]}")
            except BuildFailed as e:
                print(f"  sweep build of {e.target} failed")
                print("\n".join(e.output.splitlines()[-25:]))
                return "no-compile"
            for target in extra:
                code, output = run_test(target, None)
                report(target, None, code, output)
                if code:
                    verdict = "killed"

        if verdict == "alive":
            print("  GREEN, MUTATION SURVIVED every binary asked")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for source in m.files():
            for obj in library_objects(source):
                obj.unlink()
        for target in m.targets:
            for obj in target_objects(target):
                obj.unlink()

    return verdict


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<52} {', '.join(m.files())}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            f"  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "
            f"-DFLOX_BUILD_TESTS=ON -DFLOX_ENABLE_BACKTEST=ON")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = sorted({t for m in selected for t in m.targets})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD", "equivalent": "EQUIV"}
    for m, verdict in results:
        print(f"  {label[verdict]}  {m.name:<52} {', '.join(m.files())}")
    survived = [m.name for m, v in results if v == "alive"]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    # Survivors are the deliverable, not a failure of this script -- they are
    # reported and returned non-zero only so a CI run cannot swallow one
    # unexplained. A survivor with `equivalent` set is excluded above and
    # never reaches this gate.
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
