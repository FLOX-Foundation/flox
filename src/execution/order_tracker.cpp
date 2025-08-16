#include "flox/execution/order_tracker.h"
#include "flox/log/log.h"
#include "flox/util/performance/profile.h"

namespace flox
{

OrderTracker::OrderTracker() = default;

OrderTracker::Slot* OrderTracker::find(OrderId id)
{
  FLOX_PROFILE_SCOPE("OrderTracker::find");

  std::size_t base = id % SIZE;
  for (std::size_t i = 0; i < SIZE; ++i)
  {
    std::size_t idx = (base + i) % SIZE;
    if (_slots[idx].id.load(std::memory_order_acquire) == id)
    {
      return &_slots[idx];
    }
  }

  return nullptr;
}

OrderTracker::Slot* OrderTracker::insert(OrderId id)
{
  FLOX_PROFILE_SCOPE("OrderTracker::insert");

  std::size_t base = id % SIZE;
  for (std::size_t i = 0; i < SIZE; ++i)
  {
    std::size_t idx = (base + i) % SIZE;
    OrderId expected = 0;
    if (_slots[idx].id.compare_exchange_strong(expected, id, std::memory_order_acq_rel))
    {
      return &_slots[idx];
    }
  }

  FLOX_LOG_ERROR("[OrderTracker] Failed to insert orderId=" << id << ", tracker full.");
  std::terminate();
}

void OrderTracker::onSubmitted(const Order& order, std::string_view exchangeOrderId, std::string_view clientOrderId)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onSubmitted");

  auto* slot = insert(order.id);
  slot->state.localOrder = order;
  slot->state.exchangeOrderId = std::string(exchangeOrderId);
  slot->state.clientOrderId = std::string(clientOrderId);
  slot->state.status.store(OrderEventStatus::SUBMITTED, std::memory_order_release);
  slot->state.createdAt = now();
  slot->state.lastUpdate.store(slot->state.createdAt, std::memory_order_release);
}

void OrderTracker::onFilled(OrderId id, Quantity fill)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onFilled");

  auto* slot = find(id);
  if (!slot)
  {
    return;
  }

  const double prev = slot->state.filled.load(std::memory_order_relaxed).toDouble();
  const double next = prev + fill.toDouble();
  slot->state.filled.store(Quantity::fromDouble(next), std::memory_order_relaxed);

  slot->state.lastUpdate.store(now(), std::memory_order_release);

  const double target = slot->state.localOrder.quantity.toDouble();
  if (next >= target)
  {
    slot->state.status.store(OrderEventStatus::FILLED, std::memory_order_release);
  }
  else
  {
    slot->state.status.store(OrderEventStatus::PARTIALLY_FILLED, std::memory_order_release);
  }
}

void OrderTracker::onCanceled(OrderId id)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onCanceled");

  auto* slot = find(id);
  if (!slot)
  {
    return;
  }

  slot->state.status.store(OrderEventStatus::CANCELED, std::memory_order_release);
  slot->state.lastUpdate.store(now(), std::memory_order_release);
}

void OrderTracker::onRejected(OrderId id, std::string_view reason)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onRejected");

  auto* slot = find(id);
  if (!slot)
  {
    return;
  }

  slot->state.status.store(OrderEventStatus::REJECTED, std::memory_order_release);
  slot->state.lastUpdate.store(now(), std::memory_order_release);

  FLOX_LOG_ERROR("[OrderTracker] Order " << id << " rejected: " << reason);
}

void OrderTracker::onReplaced(OrderId oldId, const Order& newOrder, std::string_view newExchangeId, std::string_view newClientOrderId)
{
  FLOX_PROFILE_SCOPE("OrderTracker::onReplaced");

  auto* oldSlot = find(oldId);
  if (oldSlot)
  {
    oldSlot->state.status.store(OrderEventStatus::REPLACED, std::memory_order_release);
    oldSlot->state.lastUpdate.store(now(), std::memory_order_release);
  }

  auto* newSlot = insert(newOrder.id);
  newSlot->state.localOrder = newOrder;
  newSlot->state.exchangeOrderId = std::string(newExchangeId);
  newSlot->state.clientOrderId = std::string(newClientOrderId);
  newSlot->state.status.store(OrderEventStatus::SUBMITTED, std::memory_order_release);
  newSlot->state.createdAt = now();
  newSlot->state.lastUpdate.store(newSlot->state.createdAt, std::memory_order_release);
}

const OrderState* OrderTracker::get(OrderId id) const
{
  FLOX_PROFILE_SCOPE("OrderTracker::get");

  auto* slot = const_cast<OrderTracker*>(this)->find(id);
  if (!slot)
  {
    return nullptr;
  }

  return &slot->state;
}

}  // namespace flox
