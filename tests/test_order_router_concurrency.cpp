/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Review finding 21 behind the execution tracker fix.
//
// OrderRouter has no synchronisation. _enabled is a plain std::array<bool>
// written by setEnabled() from a control thread while route() reads it on a
// strategy thread, and _rrIndex is a plain `mutable size_t` incremented from
// the const selectRoundRobin(), so two routing threads read-modify-write it
// without any ordering. Both are data races under the memory model.
//
// The first two tests exercise exactly those two overlaps and are TSan-red on
// the current tree; the third is a behavioural guard that holds with or
// without a sanitizer. Run the binary from a build configured with
// -fsanitize=thread and ThreadSanitizer reports
//   WARNING: ThreadSanitizer: data race
//     Write of size 1 at ... OrderRouter<4ul>::setEnabled
//     Previous read of size 1 at ... OrderRouter<4ul>::route
// and
//   WARNING: ThreadSanitizer: data race
//     Write of size 8 at ... OrderRouter<4ul>::selectRoundRobin
// With TSAN_OPTIONS=halt_on_error=1 the process aborts and the test fails.
//
// The assertions also stand on their own without a sanitizer: every accepted
// route reaches exactly one executor, round robin visits every enabled
// destination, and a destination that is disabled and stays disabled receives
// nothing afterwards.

#include "flox/execution/order_router.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

using namespace flox;

namespace
{

constexpr int64_t kPriceRaw = 50'000LL * 100'000'000LL;
constexpr int64_t kQtyRaw = 100'000'000LL;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#endif

class CountingExecutor : public IRoutableExecutor
{
 public:
  // Deliberately not marked `override`: whichever of the two signatures
  // IRoutableExecutor declares, this class implements it, so the file keeps
  // compiling once the typed interface of finding 22 lands.
  void submit(SymbolId, Side, int64_t, int64_t, OrderId)
  {
    submits.fetch_add(1, std::memory_order_relaxed);
  }

  void submit(SymbolId, Side, Price, Quantity, OrderId)
  {
    submits.fetch_add(1, std::memory_order_relaxed);
  }

  void cancel(OrderId) override { cancels.fetch_add(1, std::memory_order_relaxed); }

  std::atomic<int> submits{0};
  std::atomic<int> cancels{0};
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

template <typename Router>
RoutingError routeOne(Router& router, SymbolId symbol, Side side, OrderId id)
{
  if constexpr (requires { router.route(symbol, side, Price::fromRaw(kPriceRaw), Quantity::fromRaw(kQtyRaw), id); })
  {
    return router.route(symbol, side, Price::fromRaw(kPriceRaw), Quantity::fromRaw(kQtyRaw), id);
  }
  else
  {
    return router.route(symbol, side, kPriceRaw, kQtyRaw, id);
  }
}

template <typename Router>
RoutingError routeOneTo(Router& router, ExchangeId exchange, SymbolId symbol, Side side, OrderId id)
{
  if constexpr (requires { router.routeTo(exchange, symbol, side, Price::fromRaw(kPriceRaw), Quantity::fromRaw(kQtyRaw), id); })
  {
    return router.routeTo(exchange, symbol, side, Price::fromRaw(kPriceRaw), Quantity::fromRaw(kQtyRaw), id);
  }
  else
  {
    return router.routeTo(exchange, symbol, side, kPriceRaw, kQtyRaw, id);
  }
}

}  // namespace

// Race on _enabled: setEnabled() writes the flag a concurrent route() reads.
TEST(OrderRouterConcurrencyTest, RouteRacesWithSetEnabled)
{
  OrderRouter<4> router;
  CountingExecutor e0;
  CountingExecutor e1;

  router.registerExecutor(0, &e0);
  router.registerExecutor(1, &e1);
  router.setRoutingStrategy(RoutingStrategy::RoundRobin);
  router.setFailoverPolicy(FailoverPolicy::Reject);

  constexpr int kRoutes = 50'000;

  std::atomic<bool> stop{false};
  std::atomic<bool> go{false};
  std::thread control(
      [&]
      {
        while (!go.load(std::memory_order_relaxed))
        {
        }
        while (!stop.load(std::memory_order_relaxed))
        {
          router.setEnabled(1, false);
          router.setEnabled(1, true);
        }
      });

  go.store(true, std::memory_order_relaxed);

  int accepted = 0;
  for (int i = 0; i < kRoutes; ++i)
  {
    if (routeOne(router, 1, Side::BUY, static_cast<OrderId>(i) + 1) == RoutingError::Success)
    {
      ++accepted;
    }
  }

  stop.store(true, std::memory_order_relaxed);
  control.join();

  EXPECT_GT(accepted, 0);
  EXPECT_EQ(e0.submits.load() + e1.submits.load(), accepted);
  EXPECT_TRUE(router.isEnabled(0));
}

// Race on _rrIndex: selectRoundRobin() read-modify-writes it from two routing
// threads. Every exchange is enabled throughout, so no route may be refused
// and every accepted route lands on exactly one executor.
TEST(OrderRouterConcurrencyTest, RoundRobinIndexRacesBetweenRoutingThreads)
{
  OrderRouter<4> router;
  CountingExecutor executors[4];

  for (ExchangeId ex = 0; ex < 4; ++ex)
  {
    router.registerExecutor(ex, &executors[ex]);
  }
  router.setRoutingStrategy(RoutingStrategy::RoundRobin);
  router.setFailoverPolicy(FailoverPolicy::Reject);
  ASSERT_EQ(router.enabledCount(), 4u);

  constexpr int kRoutesPerThread = 25'000;
  std::atomic<int> accepted{0};

  std::atomic<bool> go{false};
  auto worker = [&](OrderId base)
  {
    while (!go.load(std::memory_order_relaxed))
    {
    }
    int local = 0;
    for (int i = 0; i < kRoutesPerThread; ++i)
    {
      if (routeOne(router, 1, Side::BUY, base + static_cast<OrderId>(i)) == RoutingError::Success)
      {
        ++local;
      }
    }
    accepted.fetch_add(local, std::memory_order_relaxed);
  };

  std::thread a(worker, 1);
  std::thread b(worker, 1'000'000);
  go.store(true, std::memory_order_relaxed);
  a.join();
  b.join();

  EXPECT_EQ(accepted.load(), 2 * kRoutesPerThread);

  int total = 0;
  for (auto& executor : executors)
  {
    EXPECT_GT(executor.submits.load(), 0) << "round robin skipped an enabled destination";
    total += executor.submits.load();
  }
  EXPECT_EQ(total, 2 * kRoutesPerThread);
}

// A destination that is disabled while a routing thread hammers it must stop
// receiving orders the moment the disable is observable, and must never take
// another one afterwards.
TEST(OrderRouterConcurrencyTest, DisabledDestinationTakesNothingAfterTheDisable)
{
  OrderRouter<4> router;
  CountingExecutor e0;
  CountingExecutor e1;

  router.registerExecutor(0, &e0);
  router.registerExecutor(1, &e1);
  router.setFailoverPolicy(FailoverPolicy::Reject);

  std::atomic<bool> disabled{false};
  std::atomic<bool> stop{false};

  std::thread control(
      [&]
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        router.setEnabled(1, false);
        disabled.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed))
        {
          router.setEnabled(1, false);
        }
      });

  OrderId id = 1;
  while (!disabled.load(std::memory_order_acquire))
  {
    routeOneTo(router, 1, 1, Side::BUY, id++);
  }

  const int afterDisable = e1.submits.load();
  for (int i = 0; i < 20'000; ++i)
  {
    ASSERT_EQ(routeOneTo(router, 1, 1, Side::BUY, id++), RoutingError::ExchangeDisabled)
        << "route " << i << " after the disable";
  }

  stop.store(true, std::memory_order_relaxed);
  control.join();

  EXPECT_EQ(e1.submits.load(), afterDisable);
  EXPECT_FALSE(router.isEnabled(1));
}

// route() must never dereference an executor pointer it has not checked. The
// pointer is re-read after the enabled check for that reason: a control thread
// can null it out between the two. Unregistering in a loop against a routing
// thread has to stay TSan-clean and crash-free, and every route has to end in
// a submit on a live executor or in an error - never in a call through a
// pointer that was already withdrawn.
TEST(OrderRouterConcurrencyTest, UnregisterRacesWithRoute)
{
  OrderRouter<4> router;
  CountingExecutor executor;

  router.registerExecutor(0, &executor);
  router.setRoutingStrategy(RoutingStrategy::RoundRobin);
  router.setFailoverPolicy(FailoverPolicy::Reject);

  constexpr int kRoutes = 50'000;

  std::atomic<bool> stop{false};
  std::atomic<bool> go{false};
  std::thread control(
      [&]
      {
        while (!go.load(std::memory_order_relaxed))
        {
        }
        while (!stop.load(std::memory_order_relaxed))
        {
          router.registerExecutor(0, nullptr);
          router.registerExecutor(0, &executor);
        }
      });

  go.store(true, std::memory_order_relaxed);

  int accepted = 0;
  int refused = 0;
  for (int i = 0; i < kRoutes; ++i)
  {
    const auto err = routeOne(router, 1, Side::BUY, static_cast<OrderId>(i) + 1);
    if (err == RoutingError::Success)
    {
      ++accepted;
    }
    else
    {
      ++refused;
      ASSERT_TRUE(err == RoutingError::NoExecutor || err == RoutingError::ExchangeDisabled)
          << "route " << i << " returned an unexpected error";
    }
  }

  stop.store(true, std::memory_order_relaxed);
  control.join();

  EXPECT_EQ(accepted + refused, kRoutes);
  EXPECT_EQ(executor.submits.load(), accepted);
}

// The same contract without the threads: once a destination is unregistered,
// route() and routeTo() report an error and the executor is not called.
TEST(OrderRouterConcurrencyTest, UnregisteredDestinationReportsAnErrorInsteadOfCalling)
{
  OrderRouter<4> router;
  CountingExecutor executor;

  router.registerExecutor(0, &executor);
  router.setRoutingStrategy(RoutingStrategy::RoundRobin);
  router.setFailoverPolicy(FailoverPolicy::Reject);

  ASSERT_EQ(routeOne(router, 1, Side::BUY, 1), RoutingError::Success);
  ASSERT_EQ(executor.submits.load(), 1);

  router.registerExecutor(0, nullptr);

  EXPECT_FALSE(router.isEnabled(0));
  EXPECT_EQ(router.enabledCount(), 0u);
  EXPECT_EQ(routeOne(router, 1, Side::BUY, 2), RoutingError::NoExecutor);
  EXPECT_EQ(routeOneTo(router, 0, 1, Side::BUY, 3), RoutingError::NoExecutor);
  EXPECT_EQ(router.cancelOn(0, 3), RoutingError::NoExecutor);
  EXPECT_EQ(executor.submits.load(), 1);
  EXPECT_EQ(executor.cancels.load(), 0);

  // Failover has nothing to fail over to, and must not fall back on the
  // pointer it probed before the enabled check.
  router.setFailoverPolicy(FailoverPolicy::FailoverToBest);
  EXPECT_EQ(routeOne(router, 1, Side::BUY, 4), RoutingError::NoExecutor);
  EXPECT_EQ(executor.submits.load(), 1);
}

// An exchange id outside [0, MaxExchanges) is refused, not written past the
// end of the destination arrays. The bounds check in setEnabled() is the only
// thing standing between a caller's bad id and an out-of-range store, so the
// state of every valid destination has to survive the attempt untouched.
TEST(OrderRouterConcurrencyTest, OutOfRangeDestinationIsRefusedAndChangesNothing)
{
  OrderRouter<4> router;
  CountingExecutor executors[4];

  for (ExchangeId ex = 0; ex < 4; ++ex)
  {
    router.registerExecutor(ex, &executors[ex]);
  }
  router.setFailoverPolicy(FailoverPolicy::Reject);
  ASSERT_EQ(router.enabledCount(), 4u);

  router.setEnabled(4, true);
  router.setEnabled(4, false);
  router.setEnabled(5, true);
  router.registerExecutor(4, &executors[0]);
  router.setEnabled(InvalidExchangeId, true);

  EXPECT_EQ(router.enabledCount(), 4u);
  EXPECT_FALSE(router.isEnabled(4));
  EXPECT_FALSE(router.isEnabled(InvalidExchangeId));
  for (ExchangeId ex = 0; ex < 4; ++ex)
  {
    EXPECT_TRUE(router.isEnabled(ex)) << "destination " << ex << " changed state";
  }

  EXPECT_EQ(routeOneTo(router, 4, 1, Side::BUY, 1), RoutingError::NoExecutor);
  EXPECT_EQ(routeOneTo(router, InvalidExchangeId, 1, Side::BUY, 2), RoutingError::NoExecutor);
  EXPECT_EQ(router.cancelOn(4, 1), RoutingError::NoExecutor);

  for (auto& executor : executors)
  {
    EXPECT_EQ(executor.submits.load(), 0);
    EXPECT_EQ(executor.cancels.load(), 0);
  }

  // The last valid destination is the one an off-by-one store would land next
  // to; routing still reaches it.
  router.setRoutingStrategy(RoutingStrategy::RoundRobin);
  ASSERT_EQ(routeOneTo(router, 3, 1, Side::BUY, 3), RoutingError::Success);
  EXPECT_EQ(executors[3].submits.load(), 1);
}

// The same bounds check, made observable. An out-of-range id is not just
// "refused": without the check, setEnabled() stores through _enabled[id],
// which for a large id is memory outside the router entirely. The router is
// placed at the front of a heap block whose remainder is a known byte
// pattern, so the store that the check prevents has somewhere to land and the
// test can see it. The id is sizeof(OrderRouter<4>), which puts the write past
// the end of the router and well inside the pattern whatever the member
// layout is, and keeps the (also out-of-range) _executors read inside the same
// allocation so the probe itself stays in bounds.
namespace
{

struct RouterProbe
{
  OrderRouter<4> router;
  unsigned char tail[4096];
};

constexpr unsigned char kTailFill = 0xAB;

}  // namespace

TEST(OrderRouterConcurrencyTest, OutOfRangeDestinationWritesNothingPastTheDestinationArrays)
{
  auto probe = std::make_unique<RouterProbe>();
  std::memset(probe->tail, kTailFill, sizeof(probe->tail));

  CountingExecutor executors[4];
  for (ExchangeId ex = 0; ex < 4; ++ex)
  {
    probe->router.registerExecutor(ex, &executors[ex]);
  }
  ASSERT_EQ(probe->router.enabledCount(), 4u);

  const auto outOfRange = static_cast<ExchangeId>(sizeof(OrderRouter<4>));
  probe->router.setEnabled(outOfRange, true);
  probe->router.setEnabled(outOfRange, false);
  probe->router.registerExecutor(outOfRange, &executors[0]);

  for (size_t i = 0; i < sizeof(probe->tail); ++i)
  {
    ASSERT_EQ(probe->tail[i], kTailFill)
        << "an out-of-range destination id wrote " << i << " byte(s) past the router";
  }

  EXPECT_EQ(probe->router.enabledCount(), 4u);
  for (ExchangeId ex = 0; ex < 4; ++ex)
  {
    EXPECT_TRUE(probe->router.isEnabled(ex)) << "destination " << ex << " changed state";
  }
}
