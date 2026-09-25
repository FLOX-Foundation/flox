/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The Hyperliquid executor calls tryAcquire() with no onRejected callback
 * on all three order paths, so a submit, cancel or replace that the
 * client-side limiter refuses returns silently: no transport call, no
 * OrderEvent, and -- for a cancel -- an OrderTracker that goes on
 * reporting the order as live. That is the exact failure the onRejected
 * overload of tryAcquire was added for, and the one Bitget already wires
 * up through publishRateLimited().
 *
 * hl_sign_with_sdk() is shadowed here by the same link seam
 * unit_test_order_serialization_hyperliquid.cpp uses: this translation
 * unit's definition satisfies the linker before hl_signer.cpp.o is
 * searched, so no signing helper and no socket are involved.
 */

#include "flox-connectors/hyperliquid/hl_signer.h"
#include "flox-connectors/hyperliquid/hyperliquid_order_executor.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace flox::hl
{
std::optional<HlSig> hl_sign_with_sdk(const HlSignParams&)
{
  return HlSig{"0xdeadbeef", "0xcafebabe", 27};
}
}  // namespace flox::hl

using namespace flox;

namespace
{

// The executor has a transport-injecting constructor and a rate-limited
// constructor, but no constructor that takes both -- and the rate-limited
// one builds a real CurlTransport and fetches the asset map from
// api.hyperliquid.xyz before it returns, which is not something an offline
// test can use. The tests below are written against the overload that is
// missing; without it they fail with the signature they need.
//
// needs: HyperliquidOrderExecutorT(std::unique_ptr<ITransport> transport,
//            std::string restUrl, std::string privateKeyHex, SymbolRegistry* registry,
//            OrderTracker* orderTracker, std::shared_ptr<ILogger> logger,
//            std::string accountAddress, std::optional<std::string> vaultAddress,
//            bool mainnet, RateLimitConfig rateLimitConfig)
//
// It has to be usable from another translation unit: an explicit
// instantiation of the class template does not instantiate its member
// templates, so the existing rate-limit constructors, declared in the header
// and defined in the .cpp, would not link here. Defining this one in the
// header (as the non-template constructors effectively are, through the
// explicit instantiations) is the straightforward way.
template <typename Exec>
inline constexpr bool kHasRateLimitedTransportCtor =
    std::is_constructible_v<Exec, std::unique_ptr<ITransport>, std::string, std::string,
                            SymbolRegistry*, OrderTracker*, std::shared_ptr<ILogger>, std::string,
                            std::optional<std::string>, bool, RateLimitConfig>;

constexpr const char* kMissingCtor =
    "no offline constructor for a rate-limited Hyperliquid executor. needs: "
    "HyperliquidOrderExecutorT(std::unique_ptr<ITransport>, std::string restUrl, "
    "std::string privateKeyHex, SymbolRegistry*, OrderTracker*, std::shared_ptr<ILogger>, "
    "std::string accountAddress, std::optional<std::string> vaultAddress, bool mainnet, "
    "RateLimitConfig)";

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_hl_rate_limit_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  AtomicLoggerOptions opts;
  opts.directory = tempLogDir();
  opts.basename = name;
  return std::make_shared<AtomicLogger>(opts);
}

// Answers the asset-map fetch the constructor makes and records everything
// else, so an order call can be told apart from the meta round-trip.
class FakeTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    if (url.ends_with("/info"))
    {
      if (onSuccess)
      {
        onSuccess(R"({"universe":[{"name":"BTC"}]})");
      }
      return;
    }
    calls.push_back(std::string(body));
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<std::string> calls;
  std::string nextResponse =
      R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":777}}]}}})";
};

class RejectionWatcher final : public IOrderExecutionListener
{
 public:
  RejectionWatcher() : IOrderExecutionListener(11) {}

  void onOrderRejected(const Order& order, const std::string& reason) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _rejected.push_back({order.id, reason});
  }

  struct Rejection
  {
    OrderId id;
    std::string reason;
  };

  std::vector<Rejection> rejected()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _rejected;
  }

 private:
  std::mutex _m;
  std::vector<Rejection> _rejected;
};

RateLimitConfig limitConfig(uint32_t capacity, uint32_t refillRate)
{
  RateLimitConfig cfg;
  cfg.capacity = capacity;
  cfg.refillRate = refillRate;
  cfg.policy = RateLimitPolicy::REJECT;
  return cfg;
}

Order makeOrder(OrderId id, SymbolId symbol)
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

// Everything the three tests share, so the constructor under test appears
// once. Templated on the executor so the body is only instantiated when the
// constructor it needs exists.
template <typename Exec>
struct Harness
{
  std::unique_ptr<FakeTransport> owner{std::make_unique<FakeTransport>()};
  FakeTransport* transport{owner.get()};
  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId symbol{0};
  OrderExecutionBus bus;
  RejectionWatcher watcher;
  std::unique_ptr<Exec> executor;

  explicit Harness(const char* logName, RateLimitConfig cfg)
  {
    SymbolInfo info;
    info.exchange = "hyperliquid";
    info.symbol = "BTC";
    info.type = InstrumentType::Future;
    symbol = registry.registerSymbol(info);

    executor = std::make_unique<Exec>(std::move(owner), std::string("https://unused.invalid/order"),
                                      std::string("0x01"), &registry, &tracker, makeLogger(logName),
                                      std::string("0xaccount"), std::optional<std::string>{}, false,
                                      std::move(cfg));

    bus.subscribe(&watcher);
    bus.start();
    executor->setOrderBus(&bus);
  }

  ~Harness() { bus.stop(); }
};

template <typename Exec>
void rejectedSubmitIsReported()
{
  if constexpr (kHasRateLimitedTransportCtor<Exec>)
  {
    Harness<Exec> h("hl_rl_submit.log", limitConfig(1, 1));

    h.executor->submitOrder(makeOrder(1, h.symbol));
    h.executor->submitOrder(makeOrder(2, h.symbol));
    h.bus.flush();

    ASSERT_EQ(h.transport->calls.size(), 1u) << "the second order must not reach the venue";

    const auto rejected = h.watcher.rejected();
    ASSERT_EQ(rejected.size(), 1u)
        << "a submit the client-side limiter refused produced no event at all";
    EXPECT_EQ(rejected[0].id, 2u);
  }
  else
  {
    FAIL() << kMissingCtor;
  }
}

template <typename Exec>
void rejectedCancelIsReported()
{
  if constexpr (kHasRateLimitedTransportCtor<Exec>)
  {
    Harness<Exec> h("hl_rl_cancel.log", limitConfig(1, 1));

    h.executor->submitOrder(makeOrder(1, h.symbol));
    ASSERT_EQ(h.transport->calls.size(), 1u);

    h.executor->cancelOrder(1);
    h.bus.flush();

    EXPECT_EQ(h.transport->calls.size(), 1u);

    const auto rejected = h.watcher.rejected();
    ASSERT_EQ(rejected.size(), 1u)
        << "the cancel never left the process and nothing said so -- the tracker still reports "
           "order 1 as live";
    EXPECT_EQ(rejected[0].id, 1u);
  }
  else
  {
    FAIL() << kMissingCtor;
  }
}

template <typename Exec>
void rejectedReplaceIsReported()
{
  if constexpr (kHasRateLimitedTransportCtor<Exec>)
  {
    Harness<Exec> h("hl_rl_replace.log", limitConfig(1, 1));

    h.executor->submitOrder(makeOrder(1, h.symbol));
    ASSERT_EQ(h.transport->calls.size(), 1u);

    Order replacement = makeOrder(2, h.symbol);
    replacement.price = Price::fromDouble(60500.0);
    h.executor->replaceOrder(1, replacement);
    h.bus.flush();

    EXPECT_EQ(h.transport->calls.size(), 1u);
    EXPECT_EQ(h.watcher.rejected().size(), 1u)
        << "a replace the limiter refused left no trace on the bus";
  }
  else
  {
    FAIL() << kMissingCtor;
  }
}

// Control: inside the budget the submit goes out and nothing is reported as
// rejected. Green once the constructor exists; it is the counterweight to
// the three tests above, which must not be satisfiable by rejecting
// everything.
template <typename Exec>
void submitInsideTheBudgetIsNotReported()
{
  if constexpr (kHasRateLimitedTransportCtor<Exec>)
  {
    Harness<Exec> h("hl_rl_control.log", limitConfig(4, 4));

    h.executor->submitOrder(makeOrder(1, h.symbol));
    h.bus.flush();

    EXPECT_EQ(h.transport->calls.size(), 1u);
    EXPECT_TRUE(h.watcher.rejected().empty());
  }
  else
  {
    FAIL() << kMissingCtor;
  }
}

}  // namespace

TEST(HyperliquidRateLimitReject, RejectedSubmitIsReported)
{
  rejectedSubmitIsReported<HyperliquidOrderExecutorWithRateLimit>();
}

TEST(HyperliquidRateLimitReject, RejectedCancelIsReported)
{
  rejectedCancelIsReported<HyperliquidOrderExecutorWithRateLimit>();
}

TEST(HyperliquidRateLimitReject, RejectedReplaceIsReported)
{
  rejectedReplaceIsReported<HyperliquidOrderExecutorWithRateLimit>();
}

TEST(HyperliquidRateLimitReject, SubmitInsideTheBudgetIsNotReported)
{
  submitInsideTheBudgetIsNotReported<HyperliquidOrderExecutorWithRateLimit>();
}
