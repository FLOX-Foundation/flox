#!/usr/bin/env python3
"""Mutation harness for the order tracker, rate limiter, and order router fixes.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each fix -- terminal-order
handling and capacity bounds in OrderTracker, the zero-refill-rate path in
RateLimiter, and the thread-safety and typed-order-path work in OrderRouter --
one at a time, in the source, and checks that the acceptance test written for
it goes red -- and that an unmutated tree goes green before and after.

Every run is honest about the build: the mutated file's hash is printed before
and after, the target's object files are deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" or the run is refused,
and the test binary runs under a timeout. A mutation can take one or more
(old, new) edits, applied together as a single unit before the rebuild.

Three build directories, chosen per mutation:

    build          RelWithDebInfo  -- tracker and router-typed tests
    build-release   Release         -- rate limiter, NDEBUG path
    build-tsan      Debug + TSan    -- router concurrency test

Usage:

    python3 scripts/mutations/execution_tracker_router.py            # control, all mutations, control
    python3 scripts/mutations/execution_tracker_router.py --list
    python3 scripts/mutations/execution_tracker_router.py --only tracker-replaced-dropped-from-isterminal

The build directories are expected to be configured already:

    cmake -S . -B build         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON
    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release        -DFLOX_BUILD_TESTS=ON
    cmake -S . -B build-tsan    -DCMAKE_BUILD_TYPE=Debug -DFLOX_BUILD_TESTS=ON \\
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \\
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
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
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 180

BUILD_DIRS = {
    "build": REPO / "build",
    "build-release": REPO / "build-release",
    "build-tsan": REPO / "build-tsan",
}

RATE_LIMITER = "include/flox/util/rate_limiter.h"
ORDER_TRACKER_H = "include/flox/execution/order_tracker.h"
ORDER_TRACKER_CPP = "src/execution/order_tracker.cpp"
ORDER_ROUTER = "include/flox/execution/order_router.h"

TSAN_OPTIONS = "halt_on_error=1:exitcode=66"


@dataclass
class Edit:
    old: str
    new: str
    occurrence: int = 1
    expected_occurrences: int = 1


@dataclass
class Mutation:
    name: str
    why: str
    file: str
    target: str
    build: str  # key into BUILD_DIRS
    edits: list[Edit] = field(default_factory=list)
    test: str | None = None  # gtest_filter; None runs the whole binary
    expect_survive: bool = False  # this run's own prediction, printed in the table

    def __post_init__(self) -> None:
        if not self.edits:
            raise ValueError(f"{self.name}: no edits")


# ---------------------------------------------------------------------------
# Mutations
# ---------------------------------------------------------------------------

MUTATIONS: list[Mutation] = [

    # ---- rate limiter: zero refill rate --------------------------------------

    Mutation(
        name="rate-limiter-refill-zero-guard-removed",
        why="drops the early return in refill() for refillRate == 0; nsPerToken()'s own "
            "ns>0?ns:1 fallback keeps this from dividing by zero, so instead of crashing "
            "the bucket silently starts refilling on a rate that is supposed to never refill",
        file=RATE_LIMITER,
        target="test_rate_limiter_zero_refill",
        build="build",
        edits=[Edit(
            old="""    if (_refillRate == 0)
    {
      return;
    }

    auto now = Clock::now();""",
            new="""    auto now = Clock::now();""",
        )],
    ),
    Mutation(
        name="rate-limiter-refill-zero-guard-removed-release",
        why="the same break as above, run under Release (NDEBUG): "
            "the old assert-only guard used to vanish under NDEBUG, so the fix has to hold "
            "with optimisations on, not just in RelWithDebInfo",
        file=RATE_LIMITER,
        target="test_rate_limiter_zero_refill",
        build="build-release",
        edits=[Edit(
            old="""    if (_refillRate == 0)
    {
      return;
    }

    auto now = Clock::now();""",
            new="""    auto now = Clock::now();""",
        )],
    ),
    Mutation(
        name="rate-limiter-timeuntil-zero-guard-removed",
        why="drops the refillRate == 0 half of timeUntilAvailable's never-arrives guard, "
            "leaving only the tokens > capacity half",
        file=RATE_LIMITER,
        target="test_rate_limiter_zero_refill",
        build="build",
        edits=[Edit(
            old="""    if (_refillRate == 0 || tokens > _capacity)
    {
      return Duration::max();
    }""",
            new="""    if (tokens > _capacity)
    {
      return Duration::max();
    }""",
        )],
    ),
    Mutation(
        name="rate-limiter-nsPerToken-clamp-removed",
        why="drops the ns>0?ns:1 fallback in nsPerToken() that guards a refillRate above "
            "1e9 (rate faster than one token per nanosecond) from a second division by zero. "
            "ADVERSARIAL: both guards in refill()/timeUntilAvailable() already return before "
            "nsPerToken() is ever called with refillRate == 0, and none of the five acceptance "
            "files construct a limiter with refillRate > 1e9, so this clamp is never exercised",
        file=RATE_LIMITER,
        target="test_rate_limiter_zero_refill",
        build="build",
        edits=[Edit(
            old="    return ns > 0 ? ns : 1;",
            new="    return ns;",
        )],
        expect_survive=True,
    ),
    Mutation(
        name="rate-limiter-tokensToAdd-cap-removed",
        why="drops the explicit tokensToAdd cap at _capacity before the cast. "
            "ADVERSARIAL: the cap is redundant against every value the acceptance tests can "
            "produce, because the assignment a few lines down already clamps through "
            "std::min<uint64_t>(current + tokensToAdd, capacity) -- the same bound applied "
            "one statement later",
        file=RATE_LIMITER,
        target="test_rate_limiter_zero_refill",
        build="build",
        edits=[Edit(
            old="""    int64_t tokensToAdd = elapsed.count() / perToken;
    if (tokensToAdd > static_cast<int64_t>(_capacity))
    {
      tokensToAdd = static_cast<int64_t>(_capacity);
    }""",
            new="""    int64_t tokensToAdd = elapsed.count() / perToken;""",
        )],
        expect_survive=True,
    ),

    # ---- order tracker: terminal status and capacity bound ---------------------

    Mutation(
        name="tracker-replaced-dropped-from-isterminal",
        why="removes REPLACED from isTerminal(), the exact regression this fix closes",
        file=ORDER_TRACKER_H,
        target="test_order_tracker_replaced",
        build="build",
        edits=[Edit(
            old="""    return status == OrderEventStatus::FILLED || status == OrderEventStatus::CANCELED ||
           status == OrderEventStatus::REJECTED || status == OrderEventStatus::EXPIRED ||
           status == OrderEventStatus::REPLACED;""",
            new="""    return status == OrderEventStatus::FILLED || status == OrderEventStatus::CANCELED ||
           status == OrderEventStatus::REJECTED || status == OrderEventStatus::EXPIRED;""",
        )],
    ),
    Mutation(
        name="tracker-onfilled-own-terminal-list",
        why="restores onFilled()'s private CANCELED/REJECTED/EXPIRED check instead of asking "
            "isTerminal(), so a REPLACED (or FILLED) order silently accepts another fill. "
            "ADVERSARIAL: none of the five acceptance files calls onFilled() on an order that "
            "has just been replaced -- ReplacedOrderRefusesFurtherTransitions only exercises "
            "onCanceled/onExpired/onPendingCancel -- so the isTerminal() call inside onFilled "
            "is unguarded by any acceptance test",
        file=ORDER_TRACKER_CPP,
        target="test_order_tracker_replaced",
        build="build",
        edits=[Edit(
            old="""  auto& state = it->second;
  if (state.isTerminal())
  {
    FLOX_LOG_WARN("[OrderTracker] onFilled for terminal order " << id << " (status=" << static_cast<int>(state.status) << ")");
    return false;
  }""",
            new="""  auto& state = it->second;
  if (state.status == OrderEventStatus::CANCELED ||
      state.status == OrderEventStatus::REJECTED ||
      state.status == OrderEventStatus::EXPIRED)
  {
    FLOX_LOG_WARN("[OrderTracker] onFilled for terminal order " << id << " (status=" << static_cast<int>(state.status) << ")");
    return false;
  }""",
        )],
        expect_survive=True,
    ),
    Mutation(
        name="tracker-prune-before-insert-removed",
        why="removes the makeRoomLocked() capacity check from onSubmitted() entirely -- "
            "nothing bounds the map on the plain submit/cancel path any more",
        file=ORDER_TRACKER_CPP,
        target="test_order_tracker_capacity",
        build="build",
        edits=[Edit(
            old="""  if (!_orders.contains(order.id) && !makeRoomLocked())
  {
    FLOX_LOG_ERROR("[OrderTracker] Refusing orderId=" << order.id << ": tracker full at "
                                                      << _capacity << " live orders.");
    return false;
  }

  auto [it, inserted] = _orders.try_emplace(order.id);""",
            new="""  auto [it, inserted] = _orders.try_emplace(order.id);""",
        )],
    ),
    Mutation(
        name="tracker-live-orders-evicted-instead-of-refused",
        why="once pruning terminal entries is not enough, evicts an arbitrary (live) entry to "
            "make room instead of refusing the insert -- the exact policy inversion this fix's "
            "own comment states (\"live orders are never evicted\"). SURVIVES, and not for the "
            "reason it looks like: the eviction line only runs when the map is still full after "
            "pruneTerminalLocked(), and in this test's own numbers (capacity 8, 4 cancels before "
            "4 fresh submits) pruning alone always frees enough room, so the eviction branch this "
            "test's name promises to guard is dead code from that test's point of view. The gap "
            "is a missing scenario: capacity live orders, zero terminal ones, one more submit",
        file=ORDER_TRACKER_CPP,
        target="test_order_tracker_capacity",
        build="build",
        test="OrderTrackerCapacityTest.LiveOrdersAreNotEvictedToMakeRoom",
        edits=[Edit(
            old="""  pruneTerminalLocked();
  return _orders.size() < _capacity;""",
            new="""  pruneTerminalLocked();
  if (_orders.size() >= _capacity && !_orders.empty())
  {
    _orders.erase(_orders.begin());
  }
  return true;""",
        )],
        expect_survive=True,
    ),
    Mutation(
        name="tracker-onreplaced-room-before-mark",
        why="swaps the order in onReplaced(): asks makeRoomLocked() for room before marking "
            "the superseded order REPLACED, instead of after. ADVERSARIAL: at the exact "
            "capacity boundary the superseded order is still counted as live during the room "
            "check, so pruneTerminalLocked() cannot reclaim it and the tracker corrects itself "
            "one step later (settling at 2 live+terminal entries instead of 1) without ever "
            "exceeding capacity or refusing the replace -- the totalOrderCount() <= capacity "
            "and activeOrderCount() == 1 assertions in the acceptance tests never observe the "
            "difference",
        file=ORDER_TRACKER_CPP,
        target="test_order_tracker_replaced",
        build="build",
        edits=[Edit(
            old="""  auto oldIt = _orders.find(oldId);
  if (oldIt != _orders.end() && !oldIt->second.isTerminal())
  {
    oldIt->second.status = OrderEventStatus::REPLACED;
    oldIt->second.lastUpdate = now();
  }

  // Order matters: the superseded entry is terminal by now, so on a full
  // tracker an amend chain recycles its own history instead of being refused.
  // makeRoomLocked() invalidates oldIt.
  if (!_orders.contains(newOrder.id) && !makeRoomLocked())
  {
    FLOX_LOG_ERROR("[OrderTracker] Refusing replace into orderId=" << newOrder.id << ": tracker full at "
                                                                   << _capacity << " live orders.");
    return false;
  }""",
            new="""  if (!_orders.contains(newOrder.id) && !makeRoomLocked())
  {
    FLOX_LOG_ERROR("[OrderTracker] Refusing replace into orderId=" << newOrder.id << ": tracker full at "
                                                                   << _capacity << " live orders.");
    return false;
  }

  auto oldIt = _orders.find(oldId);
  if (oldIt != _orders.end() && !oldIt->second.isTerminal())
  {
    oldIt->second.status = OrderEventStatus::REPLACED;
    oldIt->second.lastUpdate = now();
  }""",
        )],
        expect_survive=True,
    ),
    Mutation(
        name="tracker-capacity-returns-constant",
        why="capacity() stops reporting the constructed capacity and reports the compiled-in "
            "default instead",
        file=ORDER_TRACKER_H,
        target="test_order_tracker_capacity",
        build="build",
        test="OrderTrackerCapacityTest.ExplicitCapacityIsReported",
        edits=[Edit(
            old="  size_t capacity() const noexcept { return _capacity; }",
            new="  size_t capacity() const noexcept { return 4096; }",
        )],
    ),
    Mutation(
        name="tracker-makeroom-off-by-one",
        why="ADVERSARIAL: makeRoomLocked()'s fast path triggers one entry early "
            "(_orders.size() + 1 < _capacity instead of _orders.size() < _capacity), so the "
            "tracker starts recycling terminal history a step sooner than the configured "
            "bound requires. The slow path recomputes the real size-vs-capacity comparison "
            "unconditionally, so the final answer -- and every capacity-bound assertion in "
            "the acceptance tests -- is unchanged; only the fast path is skipped one step "
            "early, which is not observable from outside makeRoomLocked()",
        file=ORDER_TRACKER_CPP,
        target="test_order_tracker_capacity",
        build="build",
        edits=[Edit(
            old="""  if (_orders.size() < _capacity)
  {
    return true;
  }""",
            new="""  if (_orders.size() + 1 < _capacity)
  {
    return true;
  }""",
        )],
        expect_survive=True,
    ),

    # ---- order router: thread safety and typed order path ------------------

    Mutation(
        name="router-enabled-atomic-downgraded-to-plain-bool",
        why="downgrades _enabled from std::array<std::atomic<bool>> to a plain "
            "std::array<bool>, restoring the unsynchronised read/write TSan is supposed to "
            "catch on the control-vs-routing-thread overlap",
        file=ORDER_ROUTER,
        target="test_order_router_concurrency",
        build="build-tsan",
        edits=[
            Edit(
                old="  std::array<std::atomic<bool>, MaxExchanges> _enabled{};",
                new="  std::array<bool, MaxExchanges> _enabled{};",
            ),
            Edit(
                old="      _enabled[exchange].store(executor != nullptr, std::memory_order_release);",
                new="      _enabled[exchange] = (executor != nullptr);",
            ),
            Edit(
                old="      _enabled[exchange].store(enabled && hasExecutor, std::memory_order_release);",
                new="      _enabled[exchange] = (enabled && hasExecutor);",
            ),
            Edit(
                old="    return exchange < MaxExchanges && _enabled[exchange].load(std::memory_order_acquire);",
                new="    return exchange < MaxExchanges && _enabled[exchange];",
            ),
            Edit(
                old="    if (!_enabled[target].load(std::memory_order_acquire))",
                new="    if (!_enabled[target])",
            ),
            Edit(
                old="    if (!_enabled[exchange].load(std::memory_order_acquire))",
                new="    if (!_enabled[exchange])",
            ),
            Edit(
                old="""      if (_enabled[ex].load(std::memory_order_acquire))
      {
        ++count;
      }""",
                new="""      if (_enabled[ex])
      {
        ++count;
      }""",
            ),
            Edit(
                old="      if (ask.valid && ask.exchange < MaxExchanges && _enabled[ask.exchange].load(std::memory_order_acquire))",
                new="      if (ask.valid && ask.exchange < MaxExchanges && _enabled[ask.exchange])",
            ),
            Edit(
                old="      if (bid.valid && bid.exchange < MaxExchanges && _enabled[bid.exchange].load(std::memory_order_acquire))",
                new="      if (bid.valid && bid.exchange < MaxExchanges && _enabled[bid.exchange])",
            ),
            Edit(
                old="""      if (!_enabled[ex].load(std::memory_order_acquire))
      {
        continue;
      }

      auto est = clockSync->estimate""",
                new="""      if (!_enabled[ex])
      {
        continue;
      }

      auto est = clockSync->estimate""",
            ),
            Edit(
                old="""      if (!_enabled[ex].load(std::memory_order_acquire))
      {
        continue;
      }

      int64_t size = 0;""",
                new="""      if (!_enabled[ex])
      {
        continue;
      }

      int64_t size = 0;""",
            ),
            Edit(
                old="""      const size_t idx = _rrIndex.fetch_add(1, std::memory_order_relaxed) % MaxExchanges;
      if (_enabled[idx].load(std::memory_order_acquire))
      {
        return static_cast<ExchangeId>(idx);
      }""",
                new="""      const size_t idx = _rrIndex.fetch_add(1, std::memory_order_relaxed) % MaxExchanges;
      if (_enabled[idx])
      {
        return static_cast<ExchangeId>(idx);
      }""",
            ),
            Edit(
                old="""      if (_enabled[ex].load(std::memory_order_acquire))
      {
        return static_cast<ExchangeId>(ex);
      }""",
                new="""      if (_enabled[ex])
      {
        return static_cast<ExchangeId>(ex);
      }""",
            ),
        ],
    ),
    Mutation(
        name="router-rrindex-fetch-add-replaced-by-load-store",
        why="replaces the single fetch_add probe in selectRoundRobin() with a separate "
            "load then store -- the textbook read-modify-write split between two routing "
            "threads. SURVIVES, and the TSan claim this mutation's name suggests is wrong: "
            "_rrIndex is still an atomic<size_t> and both the load and the store are proper "
            "atomic accesses, so there is no non-atomic memory conflict for TSan to report -- "
            "concurrent atomic load/store without synchronisation is logically racy (lost "
            "updates, colliding or skipped indices) but is not a formal data race under the "
            "C++ memory model, so ThreadSanitizer has nothing to flag here. It is not "
            "behaviour-visible either: the acceptance test only asserts aggregate totals "
            "(every accepted route reaches exactly one executor, every enabled destination "
            "gets at least one) and 25000 routes per thread makes an occasional lost update "
            "invisible against that bar",
        file=ORDER_ROUTER,
        target="test_order_router_concurrency",
        build="build-tsan",
        test="OrderRouterConcurrencyTest.RoundRobinIndexRacesBetweenRoutingThreads",
        edits=[Edit(
            old="      const size_t idx = _rrIndex.fetch_add(1, std::memory_order_relaxed) % MaxExchanges;",
            new="""      const size_t idx = _rrIndex.load(std::memory_order_relaxed) % MaxExchanges;
      _rrIndex.store(idx + 1, std::memory_order_relaxed);""",
        )],
        expect_survive=True,
    ),
    Mutation(
        name="router-executor-pointer-not-reread-after-enabled-check",
        why="captures the executor pointer once, at the same time as the initial "
            "null/validity probe, and reuses that stale value after the enabled check "
            "instead of re-reading it -- reverting the exact thing the fix's own comment "
            "calls out (\"Re-read after the enabled check rather than reusing the earlier "
            "probe\"). NEITHER TSan-visible NOR behaviour-visible under the acceptance "
            "tests: _executors is still an atomic, so the early read and the (removed) late "
            "read are each individually race-free and TSan has nothing to flag; and no "
            "acceptance test calls registerExecutor() concurrently with route() to actually "
            "null the pointer out in the window between the two reads, so the stale pointer "
            "this mutation reuses is never actually stale in practice",
        file=ORDER_ROUTER,
        target="test_order_router_concurrency",
        build="build-tsan",
        edits=[Edit(
            old="""    ExchangeId target = selectExchange(symbol, side);

    if (target == InvalidExchangeId || target >= MaxExchanges || !executor(target))
    {
      if (_failoverPolicy.load(std::memory_order_relaxed) == FailoverPolicy::FailoverToBest)
      {
        target = findAnyEnabled();
        if (target == InvalidExchangeId)
        {
          return RoutingError::NoExecutor;
        }
      }
      else
      {
        return RoutingError::NoExecutor;
      }
    }

    if (!_enabled[target].load(std::memory_order_acquire))
    {
      return RoutingError::ExchangeDisabled;
    }

    // Re-read after the enabled check rather than reusing the earlier probe:
    // the pointer is what the call below dereferences.
    auto* targetExecutor = executor(target);
    if (!targetExecutor)
    {
      return RoutingError::NoExecutor;
    }""",
            new="""    ExchangeId target = selectExchange(symbol, side);
    IRoutableExecutor* targetExecutor = nullptr;

    if (target == InvalidExchangeId || target >= MaxExchanges || !(targetExecutor = executor(target)))
    {
      if (_failoverPolicy.load(std::memory_order_relaxed) == FailoverPolicy::FailoverToBest)
      {
        target = findAnyEnabled();
        targetExecutor = target < MaxExchanges ? executor(target) : nullptr;
        if (target == InvalidExchangeId)
        {
          return RoutingError::NoExecutor;
        }
      }
      else
      {
        return RoutingError::NoExecutor;
      }
    }

    if (!_enabled[target].load(std::memory_order_acquire))
    {
      return RoutingError::ExchangeDisabled;
    }

    if (!targetExecutor)
    {
      return RoutingError::NoExecutor;
    }""",
        )],
        expect_survive=True,
    ),
    Mutation(
        name="router-setenabled-out-of-range-bounds-check-removed",
        why="ADVERSARIAL: drops the `exchange < MaxExchanges` bounds check from setEnabled(), "
            "which would read/write past the end of the _executors/_enabled arrays for an "
            "out-of-range exchange id. None of the five acceptance files ever calls "
            "setEnabled() with an out-of-range id, so this never triggers",
        file=ORDER_ROUTER,
        target="test_order_router_concurrency",
        build="build-tsan",
        edits=[Edit(
            old="""  void setEnabled(ExchangeId exchange, bool enabled)
  {
    if (exchange < MaxExchanges)
    {
      const bool hasExecutor = _executors[exchange].load(std::memory_order_acquire) != nullptr;
      _enabled[exchange].store(enabled && hasExecutor, std::memory_order_release);
    }
  }""",
            new="""  void setEnabled(ExchangeId exchange, bool enabled)
  {
    const bool hasExecutor = _executors[exchange].load(std::memory_order_acquire) != nullptr;
    _enabled[exchange].store(enabled && hasExecutor, std::memory_order_release);
  }""",
        )],
        expect_survive=True,
    ),
    Mutation(
        name="router-typed-route-swaps-price-and-quantity",
        why="route() keeps the typed Price/Quantity signature (so it still compiles and the "
            "static_asserts against a swapped call-site pair still hold) but swaps the raw "
            "values it forwards to the executor by round-tripping each through the other "
            "type's fromRaw()",
        file=ORDER_ROUTER,
        target="test_order_router_typed",
        build="build",
        test="OrderRouterTypedTest.RouteCarriesPriceAndQuantityRawUnchanged",
        edits=[Edit(
            old="    targetExecutor->submit(symbol, side, price, quantity, orderId);\n    return RoutingError::Success;\n  }\n\n  RoutingError routeTo(",
            new="    targetExecutor->submit(symbol, side, Price::fromRaw(quantity.raw()), Quantity::fromRaw(price.raw()), orderId);\n    return RoutingError::Success;\n  }\n\n  RoutingError routeTo(",
        )],
    ),
    Mutation(
        name="router-typed-routeto-swaps-price-and-quantity",
        why="same swap as above, on routeTo()'s call to the executor",
        file=ORDER_ROUTER,
        target="test_order_router_typed",
        build="build",
        test="OrderRouterTypedTest.RouteToCarriesPriceAndQuantityRawUnchanged",
        edits=[Edit(
            old="    targetExecutor->submit(symbol, side, price, quantity, orderId);\n    return RoutingError::Success;\n  }\n\n  RoutingError cancelOn(",
            new="    targetExecutor->submit(symbol, side, Price::fromRaw(quantity.raw()), Quantity::fromRaw(price.raw()), orderId);\n    return RoutingError::Success;\n  }\n\n  RoutingError cancelOn(",
        )],
    ),
]


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------

def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_occurrence(text: str, old: str, new: str, occurrence: int, expected: int) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(
            f"mutation anchor found {count} time(s), expected {expected}:\n  {old!r}"
        )
    start = -1
    for _ in range(occurrence):
        start = text.index(old, start + 1)
    return text[:start] + new + text[start + len(old):]


def apply_edits(text: str, edits: list[Edit]) -> str:
    for e in edits:
        text = replace_occurrence(text, e.old, e.new, e.occurrence, e.expected_occurrences)
    return text


def object_files(build: Path, target: str) -> list[Path]:
    """The target's own object files, located the way `find` would."""
    out = subprocess.run(
        ["find", str(build), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


def rebuild(build: Path, target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(build), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"rebuild of {target} in {build.name} failed:\n{output[-4000:]}")
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} in {build.name} compiled nothing -- the result would have "
            f"been a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_test(build: Path, target: str, gtest_filter: str | None, tsan: bool) -> tuple[int, str]:
    binary = build / "tests" / target
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    env = dict(os.environ)
    if tsan:
        env["TSAN_OPTIONS"] = TSAN_OPTIONS
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT, env=env)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def control(pairs: list[tuple[str, str]]) -> bool:
    """pairs: list of (build_key, target), deduplicated by caller."""
    ok = True
    for build_key, target in pairs:
        build = BUILD_DIRS[build_key]
        tsan = build_key == "build-tsan"
        for obj in object_files(build, target):
            obj.unlink()
        rebuild(build, target)
        code, output = run_test(build, target, None, tsan)
        state = "green" if code == 0 else "RED"
        summary = next((line for line in output.splitlines() if line.startswith("[==========] ")
                        and " ran." in line), "")
        print(f"  control {build_key:<14} {target:<32} {state}   {summary.strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> bool:
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    build = BUILD_DIRS[m.build]
    tsan = m.build == "build-tsan"
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  build   {m.build}")
    print(f"  sha256  before  {before}")

    mutated = apply_edits(original, m.edits)
    if mutated == original:
        raise SystemExit("mutation changed nothing")
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")
    print(f"  edits   {len(m.edits)}")

    try:
        removed = object_files(build, m.target)
        for obj in removed:
            obj.unlink()
        print(f"  removed {len(removed)} object file(s) for {m.target}")

        output = rebuild(build, m.target)
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {m.target}: {compiled} 'Building CXX' line(s)")

        code, test_output = run_test(build, m.target, m.test, tsan)
        killed = code != 0
        filt = f" --gtest_filter={m.test}" if m.test else ""
        print(f"  {m.target}{filt} -> exit {code} "
              f"({'RED, mutation killed' if killed else 'GREEN, MUTATION SURVIVED'})")
        if not killed:
            print("  ----- surviving mutation, test output (tail) -----")
            print("\n".join(test_output.splitlines()[-25:]))
        elif code not in (0,) and tsan:
            print("  ----- killed, TSan/test output (tail) -----")
            print("\n".join(test_output.splitlines()[-15:]))
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit("restore failed: the file does not hash back to its original")
        for obj in object_files(build, m.target):
            obj.unlink()

    return killed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<52} {m.build:<14} {m.target:<28} {m.test or '(whole binary)'}")
        return 0

    for key, build in BUILD_DIRS.items():
        if not (build / "CMakeCache.txt").is_file():
            raise SystemExit(f"{build} ({key}) is not configured; see the module docstring")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    pairs = sorted({(m.build, m.target) for m in selected})

    print("control run before the mutations")
    if not control(pairs):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(pairs)

    print("\nsummary")
    for m, killed in results:
        tag = "RED  " if killed else "ALIVE"
        as_predicted = (not killed) == m.expect_survive
        note = "" if as_predicted else "  (against prediction)"
        print(f"  {tag}  {m.name:<52} {m.build:<14} {m.target}{note}")
    survived = [m.name for m, killed in results if not killed]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
