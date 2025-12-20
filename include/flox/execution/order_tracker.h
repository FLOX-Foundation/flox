#pragma once

#include "flox/common.h"
#include "flox/execution/events/order_event.h"
#include "flox/execution/order.h"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace flox
{

struct OrderState
{
  Order localOrder;
  std::string exchangeOrderId;
  std::string clientOrderId;
  OrderEventStatus status{OrderEventStatus::NEW};
  Quantity filled{};

  TimePoint createdAt{};
  TimePoint lastUpdate{};

  bool isTerminal() const noexcept
  {
    return status == OrderEventStatus::FILLED || status == OrderEventStatus::CANCELED ||
           status == OrderEventStatus::REJECTED || status == OrderEventStatus::EXPIRED;
  }
};

class OrderTracker
{
 public:
  OrderTracker() = default;

  bool onSubmitted(const Order& order, std::string_view exchangeOrderId, std::string_view clientOrderId = "");
  bool onFilled(OrderId id, Quantity fill);
  bool onPendingCancel(OrderId id);
  bool onCanceled(OrderId id);
  bool onRejected(OrderId id, std::string_view reason);
  bool onReplaced(OrderId oldId, const Order& newOrder, std::string_view newExchangeId, std::string_view newClientOrderId = "");

  std::optional<OrderState> get(OrderId id) const;

  bool exists(OrderId id) const;

  bool isActive(OrderId id) const;

  std::optional<OrderEventStatus> getStatus(OrderId id) const;

  size_t activeOrderCount() const;

  size_t totalOrderCount() const;

  void pruneTerminal();

 private:
  mutable std::mutex _mutex;
  std::unordered_map<OrderId, OrderState> _orders;
};

}  // namespace flox
