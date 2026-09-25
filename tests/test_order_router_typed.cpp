/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Review finding 22 behind the execution tracker fix.
//
// IRoutableExecutor::submit(), OrderRouter::route() and OrderRouter::routeTo()
// take `int64_t priceRaw, int64_t quantityRaw`. Two adjacent int64_t
// parameters with nothing to tell them apart is precisely the swap the
// Decimal tags exist to prevent, and the order path is the one place where a
// swapped pair is unrecoverable. The order path must carry Price and Quantity.
//
// needs: virtual void IRoutableExecutor::submit(SymbolId, Side, Price, Quantity, OrderId) = 0;
// needs: RoutingError OrderRouter::route(SymbolId, Side, Price, Quantity, OrderId, ExchangeId* = nullptr);
// needs: RoutingError OrderRouter::routeTo(ExchangeId, SymbolId, Side, Price, Quantity, OrderId);

#include "flox/execution/order_router.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace flox;

namespace
{

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#endif

class TypedExecutor : public IRoutableExecutor
{
 public:
  // `override` on purpose: the typed signature has to be the virtual on
  // IRoutableExecutor, not a convenience wrapper bolted onto OrderRouter.
  void submit(SymbolId symbol, Side side, Price price, Quantity quantity, OrderId orderId) override
  {
    lastSymbol = symbol;
    lastSide = side;
    lastPrice = price;
    lastQuantity = quantity;
    lastOrderId = orderId;
    ++submitCount;
  }

  // Not marked `override`: it implements the legacy raw signature while that
  // one still exists on the interface, and is harmless once it is gone.
  void submit(SymbolId symbol, Side side, int64_t priceRaw, int64_t quantityRaw, OrderId orderId)
  {
    submit(symbol, side, Price::fromRaw(priceRaw), Quantity::fromRaw(quantityRaw), orderId);
  }

  void cancel(OrderId orderId) override
  {
    lastCancelId = orderId;
    ++cancelCount;
  }

  SymbolId lastSymbol{0};
  Side lastSide{Side::BUY};
  Price lastPrice{};
  Quantity lastQuantity{};
  OrderId lastOrderId{0};
  OrderId lastCancelId{0};
  int submitCount{0};
  int cancelCount{0};
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// Price and Quantity must not be interchangeable at the call site: a swapped
// pair has to be a compile error, which is the whole point of the typed
// signature.
template <typename Router>
concept RoutesSwappedPair = requires(Router& router) {
  router.route(SymbolId{1}, Side::BUY, Quantity::fromRaw(1), Price::fromRaw(1), OrderId{1});
};

template <typename Router>
concept RoutesToSwappedPair = requires(Router& router) {
  router.routeTo(ExchangeId{0}, SymbolId{1}, Side::BUY, Quantity::fromRaw(1), Price::fromRaw(1), OrderId{1});
};

static_assert(!RoutesSwappedPair<OrderRouter<4>>,
              "OrderRouter::route must reject a (Quantity, Price) pair");
static_assert(!RoutesToSwappedPair<OrderRouter<4>>,
              "OrderRouter::routeTo must reject a (Quantity, Price) pair");

}  // namespace

TEST(OrderRouterTypedTest, RouteCarriesPriceAndQuantityRawUnchanged)
{
  OrderRouter<4> router;
  TypedExecutor executor;
  router.registerExecutor(0, &executor);

  const Price price = Price::fromDouble(50'000.25);
  const Quantity quantity = Quantity::fromDouble(1.5);

  ExchangeId chosen = InvalidExchangeId;
  const auto err = router.route(11, Side::SELL, price, quantity, 4242, &chosen);

  EXPECT_EQ(err, RoutingError::Success);
  EXPECT_EQ(chosen, 0);
  EXPECT_EQ(executor.submitCount, 1);
  EXPECT_EQ(executor.lastSymbol, 11u);
  EXPECT_EQ(executor.lastSide, Side::SELL);
  EXPECT_EQ(executor.lastOrderId, 4242u);
  EXPECT_EQ(executor.lastPrice.raw(), price.raw());
  EXPECT_EQ(executor.lastQuantity.raw(), quantity.raw());
  EXPECT_EQ(executor.lastPrice.raw(), 5'000'025'000'000LL);
  EXPECT_EQ(executor.lastQuantity.raw(), 150'000'000LL);
}

TEST(OrderRouterTypedTest, RouteToCarriesPriceAndQuantityRawUnchanged)
{
  OrderRouter<4> router;
  TypedExecutor executor0;
  TypedExecutor executor1;
  router.registerExecutor(0, &executor0);
  router.registerExecutor(1, &executor1);

  const Price price = Price::fromRaw(1);
  const Quantity quantity = Quantity::fromRaw(9'223'372'036'854'775'807LL);

  const auto err = router.routeTo(1, 7, Side::BUY, price, quantity, 99);

  EXPECT_EQ(err, RoutingError::Success);
  EXPECT_EQ(executor0.submitCount, 0);
  EXPECT_EQ(executor1.submitCount, 1);
  EXPECT_EQ(executor1.lastPrice.raw(), 1LL);
  EXPECT_EQ(executor1.lastQuantity.raw(), 9'223'372'036'854'775'807LL);
  EXPECT_EQ(executor1.lastSymbol, 7u);
  EXPECT_EQ(executor1.lastOrderId, 99u);
}

// The typed path must keep every routing decision it had: a disabled
// destination is still refused and the executor is not called.
TEST(OrderRouterTypedTest, TypedRouteStillHonoursDisabledDestinations)
{
  OrderRouter<4> router;
  TypedExecutor executor;
  router.registerExecutor(2, &executor);
  router.setEnabled(2, false);

  const auto err = router.routeTo(2, 3, Side::BUY, Price::fromDouble(1.0), Quantity::fromDouble(1.0), 5);

  EXPECT_EQ(err, RoutingError::ExchangeDisabled);
  EXPECT_EQ(executor.submitCount, 0);
}
