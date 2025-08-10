#pragma once

#include "flox/common.h"
#include "flox/engine/engine_config.h"
#include "flox/execution/events/order_event.h"
#include "flox/execution/order.h"

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

namespace flox
{

struct OrderState
{
  Order localOrder;
  std::string exchangeOrderId;
  std::string clientOrderId;
  std::atomic<OrderEventStatus> status{OrderEventStatus::NEW};
  std::atomic<Quantity> filled = Quantity::fromDouble(0.0);

  TimePoint createdAt{};
  std::atomic<TimePoint> lastUpdate{};
};

class OrderTracker
{
 public:
  static constexpr std::size_t SIZE = config::ORDER_TRACKER_CAPACITY;

  OrderTracker();

  void onSubmitted(const Order& order, std::string_view exchangeOrderId, std::string_view clientOrderId = "");
  void onFilled(OrderId id, Quantity fill);
  void onCanceled(OrderId id);
  void onRejected(OrderId id, std::string_view reason);
  void onReplaced(OrderId oldId, const Order& newOrder, std::string_view newExchangeId, std::string_view newClientOrderId = "");

  const OrderState* get(OrderId id) const;

 private:
  struct Slot
  {
    std::atomic<OrderId> id{0};
    OrderState state;
  };

  Slot _slots[SIZE];

  Slot* find(OrderId id);
  Slot* insert(OrderId id);
};

}  // namespace flox
