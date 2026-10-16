#pragma once

#include "lrvx/common.h"
#include "lrvx/engine/engine_config.h"
#include "lrvx/execution/events/order_event.h"
#include "lrvx/execution/order.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lrvx
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

  /// REPLACED belongs here: onReplaced() writes it on the superseded order,
  /// which by definition will never report again. Leaving it out kept every
  /// amended order "active" for the life of the process, so activeOrderCount()
  /// counted dead orders and pruneTerminal() could not reclaim them.
  bool isTerminal() const noexcept
  {
    return status == OrderEventStatus::FILLED || status == OrderEventStatus::CANCELED ||
           status == OrderEventStatus::REJECTED || status == OrderEventStatus::EXPIRED ||
           status == OrderEventStatus::REPLACED;
  }
};

/// Bounded order-state map.
///
/// Capacity policy. The tracker holds at most capacity() entries. Nothing on
/// the live path called pruneTerminal(), so the tracker prunes itself: when an
/// insert (onSubmitted or onReplaced) finds the map full, terminal entries are
/// dropped to make room. History is what gets sacrificed - a live order is
/// never evicted.
///
/// If the map is full of live orders the insert is refused: onSubmitted() and
/// onReplaced() return false and log an error. Dropping a live order silently
/// would leave a working order on the venue that the process no longer knows
/// about, which is strictly worse than refusing the submit at a point where
/// the caller can still react.
class OrderTracker
{
 public:
  OrderTracker() : OrderTracker(static_cast<size_t>(config::ORDER_TRACKER_CAPACITY)) {}

  explicit OrderTracker(size_t capacity);

  size_t capacity() const noexcept { return _capacity; }

  bool onSubmitted(const Order& order, std::string_view exchangeOrderId, std::string_view clientOrderId = "");
  bool onFilled(OrderId id, Quantity fill);
  bool onPendingCancel(OrderId id);
  bool onCanceled(OrderId id);
  bool onExpired(OrderId id);
  bool onRejected(OrderId id, std::string_view reason);
  bool onReplaced(OrderId oldId, const Order& newOrder, std::string_view newExchangeId, std::string_view newClientOrderId = "");

  std::optional<OrderState> get(OrderId id) const;

  /// The local id that onSubmitted / onReplaced recorded the given exchange
  /// order id for. Venue WS reports are keyed by the venue's own id
  /// (Hyperliquid `oid`, Binance `orderId`), so fill reconciliation needs
  /// this direction; without it every adapter grows a parallel map. A
  /// terminal entry still resolves until it is pruned — late fills on a
  /// replaced order arrive keyed by the superseded id — and when a venue
  /// reuses an id, the newest holder wins.
  std::optional<OrderId> findLocalIdByExchangeOrderId(std::string_view exchangeOrderId) const;

  bool exists(OrderId id) const;

  bool isActive(OrderId id) const;

  std::optional<OrderEventStatus> getStatus(OrderId id) const;

  size_t activeOrderCount() const;

  size_t totalOrderCount() const;

  /// Drop every terminal entry. The tracker calls this itself when an insert
  /// hits the capacity bound; an owner that wants the memory back sooner (end
  /// of session, say) can still call it directly.
  void pruneTerminal();

 private:
  /// Caller holds _mutex. Returns true if there is room for one more entry.
  bool makeRoomLocked();
  void pruneTerminalLocked();

  /// Caller holds _mutex. Keep _byExchangeOrderId in step with _orders:
  /// index on insert, un-index exactly when the entry is dropped.
  void indexExchangeIdLocked(const std::string& exchangeOrderId, OrderId id);
  void unindexExchangeIdLocked(const OrderState& state, OrderId id);

  /// Heterogeneous lookup so a string_view query does not allocate a
  /// temporary std::string on the fill path.
  struct TransparentStringHash
  {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept
    {
      return std::hash<std::string_view>{}(s);
    }
  };

  mutable std::mutex _mutex;
  size_t _capacity;
  std::unordered_map<OrderId, OrderState> _orders;

  // exchangeOrderId -> local id. Bounded by the same capacity as _orders (at
  // most one entry per tracked order) and pruned with it, so the memory
  // bound the capacity promises still holds.
  std::unordered_map<std::string, OrderId, TransparentStringHash, std::equal_to<>>
      _byExchangeOrderId;
};

}  // namespace lrvx
