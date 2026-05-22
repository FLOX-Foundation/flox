/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/order_queue_tracker.h"
#include "flox/common.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox
{

struct LiveQueueSnapshot
{
  OrderId orderId{};
  Quantity queueAheadEst{};  // estimated volume ahead of our order
  Quantity total{};          // estimated current level total
  double confidence{1.0};    // [0..1], decays with time / proportional-shrink count
  int64_t lastUpdateNs{0};   // feed timestamp of the most recent attribution
};

// Client-side queue-position estimator. Exchanges normally don't
// publish per-order queue position, but the value can be
// approximated from the order book and trade tape:
//   - At placement time, record the level total at arrival.
//   - As trades print at our price level, deduct consumed volume
//     (skipping our own fills, which we are told about separately).
//   - As the level shrinks via book updates beyond what trades
//     explain, attribute the residual to cancellations using the
//     same proportional-shrink heuristic the backtest simulator uses.
//
// The position is a heuristic, not a measurement. The exact ordering
// of cancellations vs new joins is hidden, so estimates can drift.
// `confidence` reports how much of the answer came from
// trade-based attributions (high confidence) vs proportional-shrink
// attributions (lower) and decays exponentially with elapsed time.
class LiveQueuePositionEstimator
{
 public:
  LiveQueuePositionEstimator()
  {
    _tracker.setModel(QueueModel::FULL, /*depth=*/8);
  }

  // Confidence decays toward 0 as `exp(-elapsed / halfLife)`.
  // Zero or negative disables the time decay. Default ≈ 60 seconds.
  void setConfidenceHalfLifeNs(int64_t halfLifeNs) noexcept
  {
    _halfLifeNs = halfLifeNs;
  }
  int64_t confidenceHalfLifeNs() const noexcept { return _halfLifeNs; }

  // Each proportional-shrink attribution multiplies the order's
  // confidence by this factor (per shrink event). Defaults to 0.85.
  void setShrinkAttributionFactor(double factor) noexcept
  {
    _shrinkFactor = factor;
  }
  double shrinkAttributionFactor() const noexcept { return _shrinkFactor; }

  // Order lifecycle ------------------------------------------------

  void onOrderPlaced(SymbolId symbol, Side side, Price levelPrice, OrderId orderId,
                     Quantity orderQty, Quantity levelQtyNow, int64_t tsNs)
  {
    _tracker.addOrder(symbol, side, levelPrice, orderId, orderQty, levelQtyNow);
    OrderState st{};
    st.placedNs = tsNs;
    st.lastUpdateNs = tsNs;
    st.confidence = 1.0;
    _orders[orderId] = st;
  }

  void onOrderCancelled(OrderId orderId, int64_t /*tsNs*/)
  {
    _tracker.removeOrder(orderId);
    _orders.erase(orderId);
  }

  // Called when our resting order receives a fill — this volume is
  // taken from the queue but is ours, not "ahead of us". The
  // OrderQueueTracker handles the bookkeeping when we feed the
  // trade via onTrade(), but the live estimator also wants to know
  // when the order has fully filled so it can drop the entry.
  void onOrderFilled(OrderId orderId, Quantity cumulativeFill, int64_t tsNs)
  {
    auto it = _orders.find(orderId);
    if (it == _orders.end())
    {
      return;
    }
    it->second.lastUpdateNs = tsNs;
    auto snap = _tracker.snapshot(orderId);
    if (!snap.has_value() || snap->total.raw() == 0 ||
        cumulativeFill.raw() >= snap->total.raw())
    {
      // Fully filled (or no longer tracked) — drop.
      _tracker.removeOrder(orderId);
      _orders.erase(it);
    }
  }

  // Market events --------------------------------------------------

  void onTrade(SymbolId symbol, Price price, Quantity tradeQty, int64_t tsNs)
  {
    std::vector<std::pair<OrderId, Quantity>> filled;
    _tracker.onTrade(symbol, price, tradeQty, filled);
    for (const auto& f : filled)
    {
      auto it = _orders.find(f.first);
      if (it != _orders.end())
      {
        it->second.lastUpdateNs = tsNs;
        // Trade-attributed deductions don't drop confidence — we
        // know what happened. (They DO drop queue_ahead, via the
        // tracker.)
      }
    }
    // Trades at our price level also reduce queue-ahead for orders
    // sitting on that level even if they're not the fill recipient,
    // because the tracker subtracted the trade qty from the level
    // total. Bump lastUpdateNs for any orders on the touched level.
    touchOrdersAtPrice(symbol, price, tsNs);
  }

  void onLevelUpdate(SymbolId symbol, Side side, Price price, Quantity newQty,
                     int64_t tsNs)
  {
    // We don't see the tracker's internal state delta — if its level
    // shrank beyond what trades explained, that's a cancellation
    // attribution. Approximate by capturing each tracked order's
    // total before the update and comparing after; any order whose
    // level total decreased gets one shrink-attribution confidence
    // hit.
    std::unordered_map<OrderId, int64_t> preTotals;
    preTotals.reserve(_orders.size());
    for (const auto& [oid, _] : _orders)
    {
      auto snap = _tracker.snapshot(oid);
      preTotals[oid] = snap.has_value() ? snap->total.raw() : 0;
    }
    _tracker.onLevelUpdate(symbol, side, price, newQty);
    for (auto& [oid, state] : _orders)
    {
      auto snap = _tracker.snapshot(oid);
      int64_t post = snap.has_value() ? snap->total.raw() : 0;
      if (post < preTotals[oid])
      {
        state.confidence *= _shrinkFactor;
        state.lastUpdateNs = tsNs;
      }
    }
  }

  // Read -----------------------------------------------------------

  std::optional<LiveQueueSnapshot> snapshot(OrderId orderId,
                                            int64_t nowNs = 0) const
  {
    auto it = _orders.find(orderId);
    if (it == _orders.end())
    {
      return std::nullopt;
    }
    auto q = _tracker.snapshot(orderId);
    if (!q.has_value())
    {
      return std::nullopt;
    }
    LiveQueueSnapshot out{};
    out.orderId = orderId;
    out.queueAheadEst = q->ahead;
    out.total = q->total;
    out.confidence = it->second.confidence;
    out.lastUpdateNs = it->second.lastUpdateNs;
    if (_halfLifeNs > 0 && nowNs > it->second.placedNs)
    {
      const double elapsed = static_cast<double>(nowNs - it->second.placedNs);
      const double decay = std::exp(-elapsed * 0.69314718 / static_cast<double>(_halfLifeNs));
      out.confidence *= decay;
    }
    if (out.confidence < 0.0)
    {
      out.confidence = 0.0;
    }
    if (out.confidence > 1.0)
    {
      out.confidence = 1.0;
    }
    return out;
  }

  size_t trackedOrderCount() const noexcept { return _orders.size(); }

 private:
  struct OrderState
  {
    int64_t placedNs{0};
    int64_t lastUpdateNs{0};
    double confidence{1.0};
  };

  void touchOrdersAtPrice(SymbolId /*symbol*/, Price /*price*/, int64_t tsNs)
  {
    for (auto& [oid, state] : _orders)
    {
      auto snap = _tracker.snapshot(oid);
      if (!snap.has_value())
      {
        continue;
      }
      state.lastUpdateNs = tsNs;
    }
  }

  OrderQueueTracker _tracker;
  std::unordered_map<OrderId, OrderState> _orders;
  int64_t _halfLifeNs{60'000'000'000LL};  // 60s default
  double _shrinkFactor{0.85};
};

}  // namespace flox
