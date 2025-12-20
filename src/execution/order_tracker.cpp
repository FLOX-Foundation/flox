#include "flox/execution/order_tracker.h"
#include "flox/log/log.h"
#include "flox/util/performance/profile.h"

namespace flox
{

bool OrderTracker::onSubmitted(const Order& order, std::string_view exchangeOrderId, std::string_view clientOrderId)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onSubmitted");

  std::lock_guard<std::mutex> lock(_mutex);

  auto [it, inserted] = _orders.try_emplace(order.id);
  if (!inserted)
  {
    FLOX_LOG_WARN("[OrderTracker] Duplicate orderId=" << order.id << ", ignoring insert.");
    return false;
  }

  auto& state = it->second;
  state.localOrder = order;
  state.exchangeOrderId = std::string(exchangeOrderId);
  state.clientOrderId = std::string(clientOrderId);
  state.filled = Quantity{};
  state.status = OrderEventStatus::SUBMITTED;
  state.createdAt = now();
  state.lastUpdate = state.createdAt;
  return true;
}

bool OrderTracker::onFilled(OrderId id, Quantity fill)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onFilled");

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _orders.find(id);
  if (it == _orders.end())
  {
    FLOX_LOG_WARN("[OrderTracker] onFilled for unknown orderId=" << id);
    return false;
  }

  auto& state = it->second;
  if (state.status == OrderEventStatus::CANCELED ||
      state.status == OrderEventStatus::REJECTED ||
      state.status == OrderEventStatus::EXPIRED)
  {
    FLOX_LOG_WARN("[OrderTracker] onFilled for terminal order " << id << " (status=" << static_cast<int>(state.status) << ")");
    return false;
  }

  state.filled = Quantity::fromRaw(state.filled.raw() + fill.raw());
  state.lastUpdate = now();

  if (state.filled.raw() >= state.localOrder.quantity.raw())
  {
    state.status = OrderEventStatus::FILLED;
  }
  else
  {
    state.status = OrderEventStatus::PARTIALLY_FILLED;
  }
  return true;
}

bool OrderTracker::onPendingCancel(OrderId id)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onPendingCancel");

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _orders.find(id);
  if (it == _orders.end())
  {
    FLOX_LOG_WARN("[OrderTracker] onPendingCancel for unknown orderId=" << id);
    return false;
  }

  auto& state = it->second;
  if (state.isTerminal())
  {
    return false;
  }

  state.status = OrderEventStatus::PENDING_CANCEL;
  state.lastUpdate = now();
  return true;
}

bool OrderTracker::onCanceled(OrderId id)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onCanceled");

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _orders.find(id);
  if (it == _orders.end())
  {
    FLOX_LOG_WARN("[OrderTracker] onCanceled for unknown orderId=" << id);
    return false;
  }

  auto& state = it->second;
  if (state.isTerminal())
  {
    return false;
  }

  state.status = OrderEventStatus::CANCELED;
  state.lastUpdate = now();
  return true;
}

bool OrderTracker::onRejected(OrderId id, std::string_view reason)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onRejected");

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _orders.find(id);
  if (it == _orders.end())
  {
    FLOX_LOG_WARN("[OrderTracker] onRejected for unknown orderId=" << id);
    return false;
  }

  auto& state = it->second;
  if (state.isTerminal())
  {
    return false;
  }

  state.status = OrderEventStatus::REJECTED;
  state.lastUpdate = now();

  FLOX_LOG_ERROR("[OrderTracker] Order " << id << " rejected: " << reason);
  return true;
}

bool OrderTracker::onReplaced(OrderId oldId, const Order& newOrder, std::string_view newExchangeId, std::string_view newClientOrderId)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onReplaced");

  std::lock_guard<std::mutex> lock(_mutex);

  auto oldIt = _orders.find(oldId);
  if (oldIt != _orders.end() && !oldIt->second.isTerminal())
  {
    oldIt->second.status = OrderEventStatus::REPLACED;
    oldIt->second.lastUpdate = now();
  }

  auto [newIt, inserted] = _orders.try_emplace(newOrder.id);
  if (!inserted)
  {
    FLOX_LOG_WARN("[OrderTracker] Duplicate orderId=" << newOrder.id << " on replace.");
    return false;
  }

  auto& state = newIt->second;
  state.localOrder = newOrder;
  state.exchangeOrderId = std::string(newExchangeId);
  state.clientOrderId = std::string(newClientOrderId);
  state.filled = Quantity{};
  state.status = OrderEventStatus::SUBMITTED;
  state.createdAt = now();
  state.lastUpdate = state.createdAt;
  return true;
}

std::optional<OrderState> OrderTracker::get(OrderId id) const
{
  FLOX_PROFILE_SCOPE("OrderTracker::get");

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _orders.find(id);
  if (it == _orders.end())
  {
    return std::nullopt;
  }
  return it->second;
}

bool OrderTracker::exists(OrderId id) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  return _orders.count(id) > 0;
}

bool OrderTracker::isActive(OrderId id) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _orders.find(id);
  return it != _orders.end() && !it->second.isTerminal();
}

std::optional<OrderEventStatus> OrderTracker::getStatus(OrderId id) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _orders.find(id);
  if (it == _orders.end())
  {
    return std::nullopt;
  }
  return it->second.status;
}

size_t OrderTracker::activeOrderCount() const
{
  std::lock_guard<std::mutex> lock(_mutex);
  size_t count = 0;
  for (const auto& [id, state] : _orders)
  {
    if (!state.isTerminal())
    {
      ++count;
    }
  }
  return count;
}

size_t OrderTracker::totalOrderCount() const
{
  std::lock_guard<std::mutex> lock(_mutex);
  return _orders.size();
}

void OrderTracker::pruneTerminal()
{
  std::lock_guard<std::mutex> lock(_mutex);
  for (auto it = _orders.begin(); it != _orders.end();)
  {
    if (it->second.isTerminal())
    {
      it = _orders.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

}  // namespace flox
