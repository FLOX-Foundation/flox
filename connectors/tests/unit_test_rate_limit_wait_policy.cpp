/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * RateLimitPolicy::WAIT makes two promises and currently keeps neither.
 *
 *  - "wait" is implemented as std::this_thread::sleep_for on whichever
 *    thread called submitOrder -- the strategy / event-bus consumer thread.
 *    One contended submit stops the engine from processing anything, market
 *    data included, for as long as the venue budget says to wait.
 *
 *  - after the sleep the retry's result is discarded ((void)tryAcquire())
 *    and true is returned regardless, so several threads that waited on the
 *    same empty bucket all proceed and the request goes out over the limit
 *    anyway -- the one outcome a client-side limiter exists to prevent.
 *
 * Both are asserted at the executor, on what actually reaches the
 * transport and when, because that is what the venue counts. Either fix
 * shape passes: the wait can move to another thread or the order can be
 * queued, as long as the caller returns and the send lands no earlier than
 * the budget allows.
 */

#include "flox-connectors/bitget/authenticated_rest_client.h"
#include "flox-connectors/bitget/bitget_order_executor.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace std::chrono_literals;

namespace
{

using Clock = std::chrono::steady_clock;

// Records when each request reached the wire, from whichever thread the
// executor ends up sending on.
class TimestampingTransport final : public ITransport
{
 public:
  void post(std::string_view, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    {
      std::lock_guard<std::mutex> lk(_m);
      _sends.push_back({Clock::now(), std::string(body)});
    }
    _cv.notify_all();
    if (onSuccess)
    {
      onSuccess(R"({"code":"00000","msg":"success","data":{"orderId":"BG-1"}})");
    }
  }

  struct Send
  {
    Clock::time_point at;
    std::string body;
  };

  std::size_t count()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _sends.size();
  }

  bool waitForCount(std::size_t n, std::chrono::milliseconds d)
  {
    std::unique_lock<std::mutex> lk(_m);
    return _cv.wait_for(lk, d,
                        [&]
                        {
                          return _sends.size() >= n;
                        });
  }

  std::vector<Send> snapshot()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _sends;
  }

 private:
  std::mutex _m;
  std::condition_variable _cv;
  std::vector<Send> _sends;
};

class RejectionWatcher final : public IOrderExecutionListener
{
 public:
  RejectionWatcher() : IOrderExecutionListener(9) {}

  void onOrderRejected(const Order& order, const std::string&) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _rejected.push_back(order.id);
  }

  std::vector<OrderId> rejected()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _rejected;
  }

 private:
  std::mutex _m;
  std::vector<OrderId> _rejected;
};

Bitget::Params baseParams()
{
  Bitget::Params p;
  p.productType = "USDT-FUTURES";
  p.marginCoin = "USDT";
  p.marginMode = "crossed";
  return p;
}

RateLimitConfig limitConfig(uint32_t capacity, uint32_t refillRate, RateLimitPolicy policy)
{
  RateLimitConfig cfg;
  cfg.capacity = capacity;
  cfg.refillRate = refillRate;
  cfg.policy = policy;
  return cfg;
}

struct Harness
{
  std::unique_ptr<TimestampingTransport> owner{std::make_unique<TimestampingTransport>()};
  TimestampingTransport* transport{owner.get()};
  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId symbol{0};
  std::unique_ptr<BitgetOrderExecutorT<WithRateLimit>> executor;

  explicit Harness(RateLimitConfig cfg)
  {
    SymbolInfo info;
    info.exchange = "bitget";
    info.symbol = "BTCUSDT";
    info.type = InstrumentType::Future;
    symbol = registry.registerSymbol(info);

    auto client = std::make_unique<BitgetAuthenticatedRestClient>(
        "k", "s", "p", "https://api.bitget.com", transport);
    executor = std::make_unique<BitgetOrderExecutorT<WithRateLimit>>(
        std::move(client), &registry, &tracker, baseParams(), std::move(cfg));
  }

  Order makeOrder(OrderId id) const
  {
    Order o;
    o.id = id;
    o.symbol = symbol;
    o.side = Side::BUY;
    o.type = OrderType::LIMIT;
    o.price = Price::fromDouble(60000.0);
    o.quantity = Quantity::fromDouble(1.0);
    return o;
  }
};

int64_t msBetween(Clock::time_point a, Clock::time_point b)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

}  // namespace

// Control, green today: inside the budget nothing is delayed and nothing is
// held back.
TEST(RateLimitWaitPolicy, SubmitsInsideTheBudgetGoOutImmediately)
{
  Harness h(limitConfig(2, 2, RateLimitPolicy::WAIT));

  const auto start = Clock::now();
  h.executor->submitOrder(h.makeOrder(1));
  h.executor->submitOrder(h.makeOrder(2));

  ASSERT_TRUE(h.transport->waitForCount(2, 2s));
  EXPECT_LE(msBetween(start, Clock::now()), 200);
}

// Budget of one token, refilling twice a second. The second submit cannot
// go out for ~500 ms -- but that is the venue's problem, not the strategy
// thread's: submitOrder must return at once and the order must still be
// sent, once the budget allows it.
TEST(RateLimitWaitPolicy, WaitDoesNotBlockTheSubmittingThread)
{
  Harness h(limitConfig(1, 2, RateLimitPolicy::WAIT));

  const auto start = Clock::now();
  h.executor->submitOrder(h.makeOrder(1));
  ASSERT_TRUE(h.transport->waitForCount(1, 2s));

  const auto beforeSecond = Clock::now();
  h.executor->submitOrder(h.makeOrder(2));
  const int64_t returned = msBetween(beforeSecond, Clock::now());

  EXPECT_LE(returned, 100) << "submitOrder() slept for " << returned
                           << " ms on the calling thread waiting for a rate-limit token";

  ASSERT_TRUE(h.transport->waitForCount(2, 3s))
      << "the rate-limited order was never sent -- WAIT must defer it, not drop it";

  const auto sends = h.transport->snapshot();
  ASSERT_EQ(sends.size(), 2u);
  EXPECT_GE(msBetween(start, sends[1].at), 400)
      << "the second order went out " << msBetween(start, sends[1].at)
      << " ms in, before the budget had refilled";
}

// The over-send. Four threads meet an empty bucket; each is told to wait
// the same interval, and every one of them proceeds afterwards whether or
// not a token was actually there. With one token and two per second, at
// most two requests may leave in the first 600 ms.
TEST(RateLimitWaitPolicy, WaitNeverSendsOverTheLimit)
{
  Harness h(limitConfig(1, 2, RateLimitPolicy::WAIT));

  const auto start = Clock::now();
  std::vector<std::thread> threads;
  for (OrderId id = 1; id <= 4; ++id)
  {
    threads.emplace_back(
        [&h, id]
        {
          h.executor->submitOrder(h.makeOrder(id));
        });
  }
  for (auto& t : threads)
  {
    t.join();
  }

  // Four submits at two tokens per second need ~1.5 s of budget.
  ASSERT_TRUE(h.transport->waitForCount(4, 5s))
      << "only " << h.transport->count() << " of 4 orders were ever sent";

  const auto sends = h.transport->snapshot();
  const auto inWindow =
      static_cast<std::size_t>(std::count_if(sends.begin(), sends.end(),
                                             [&](const TimestampingTransport::Send& s)
                                             {
                                               return msBetween(start, s.at) <= 600;
                                             }));

  EXPECT_LE(inWindow, 2u) << inWindow
                          << " requests left in the first 600 ms on a budget of 1 token plus 2 per "
                             "second -- the waiters sent without holding a token";
}

// Control, green today: REJECT already refuses the over-budget submit and
// says so on the bus. WAIT must end up no less honest about what it sent.
TEST(RateLimitWaitPolicy, RejectPolicyStillRefusesAndReportsIt)
{
  Harness h(limitConfig(1, 1, RateLimitPolicy::REJECT));

  RejectionWatcher watcher;
  OrderExecutionBus bus;
  bus.subscribe(&watcher);
  bus.start();
  h.executor->setOrderBus(&bus);

  h.executor->submitOrder(h.makeOrder(1));
  h.executor->submitOrder(h.makeOrder(2));

  ASSERT_TRUE(h.transport->waitForCount(1, 2s));
  bus.flush();
  bus.stop();

  EXPECT_EQ(h.transport->count(), 1u);
  const auto rejected = watcher.rejected();
  ASSERT_EQ(rejected.size(), 1u);
  EXPECT_EQ(rejected[0], 2u);
}
