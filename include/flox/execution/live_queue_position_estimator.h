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
  Quantity queueAheadEst{};     // estimated volume ahead of our order
  Quantity total{};             // estimated current level total
  double confidence{1.0};       // [0..1], decays with time / proportional-shrink count
  int64_t lastUpdateNs{0};      // feed timestamp of the most recent attribution
  Quantity hiddenVolumeSeen{};  // cumulative volume attributed to hidden / iceberg
};

// Policy for how the estimator handles trades that may have come from
// hidden / iceberg liquidity rather than the visible book.
enum class HiddenOrderPolicy : uint8_t
{
  // Every trade deducts visible queue. Original behaviour; over-attributes
  // when hidden flow is present on a venue.
  Ignore = 0,
  // Caller passes is_hidden flag on the trade; flagged trades do not
  // deduct queue or feed the proportional-shrink path. Use this on
  // venues that ship a per-trade `is_hidden` field (Bybit some products).
  TrustTradeFlag = 1,
  // Inference: if reported trade volume exceeds the pre-trade visible
  // level total, attribute the excess to hidden flow. Useful on venues
  // that do not flag hidden but where the math is observable.
  InferIfTradeExceedsVisible = 2,
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

  // Hidden-order attribution policy. See enum above. Default: Ignore.
  void setHiddenOrderPolicy(HiddenOrderPolicy policy) noexcept
  {
    _hiddenPolicy = policy;
  }
  HiddenOrderPolicy hiddenOrderPolicy() const noexcept { return _hiddenPolicy; }

  // Order lifecycle ------------------------------------------------

  void onOrderPlaced(SymbolId symbol, Side side, Price levelPrice, OrderId orderId,
                     Quantity orderQty, Quantity levelQtyNow, int64_t tsNs)
  {
    _tracker.addOrder(symbol, side, levelPrice, orderId, orderQty, levelQtyNow);
    OrderState st{};
    st.placedNs = tsNs;
    st.lastUpdateNs = tsNs;
    st.confidence = 1.0;
    st.symbol = symbol;
    st.side = side;
    st.levelPriceRaw = levelPrice.raw();
    _orders[orderId] = st;
    _lastLevelQty[{symbol, side, levelPrice.raw()}] = levelQtyNow.raw();
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
    onTradeImpl(symbol, price, tradeQty, tsNs, /*isHiddenFlag=*/false);
  }

  // Trade overload that carries the venue's per-trade `is_hidden` flag.
  // Behaviour depends on hiddenOrderPolicy():
  //   Ignore                       — flag ignored, trade deducts as normal
  //   TrustTradeFlag               — when true, no deduction + hidden accumulator
  //   InferIfTradeExceedsVisible   — flag ignored, inference handles it
  void onTradeWithFlag(SymbolId symbol, Price price, Quantity tradeQty, int64_t tsNs,
                       bool isHidden)
  {
    onTradeImpl(symbol, price, tradeQty, tsNs, isHidden);
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
    _lastLevelQty[{symbol, side, price.raw()}] = newQty.raw();
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
    out.hiddenVolumeSeen = Quantity::fromRaw(it->second.hiddenVolumeSeenRaw);
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
    int64_t hiddenVolumeSeenRaw{0};
    SymbolId symbol{};
    Side side{};
    int64_t levelPriceRaw{0};
  };

  struct LevelKey
  {
    SymbolId symbol;
    Side side;
    int64_t priceRaw;
    bool operator==(const LevelKey& o) const noexcept
    {
      return symbol == o.symbol && side == o.side && priceRaw == o.priceRaw;
    }
  };
  struct LevelKeyHash
  {
    size_t operator()(const LevelKey& k) const noexcept
    {
      return std::hash<int64_t>{}(k.priceRaw) ^
             (static_cast<size_t>(k.symbol) << 1) ^
             (static_cast<size_t>(k.side) << 16);
    }
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

  void onTradeImpl(SymbolId symbol, Price price, Quantity tradeQty, int64_t tsNs,
                   bool isHiddenFlag)
  {
    int64_t visibleTradeRaw = tradeQty.raw();
    int64_t hiddenAttributedRaw = 0;

    if (_hiddenPolicy == HiddenOrderPolicy::TrustTradeFlag && isHiddenFlag)
    {
      hiddenAttributedRaw = tradeQty.raw();
      visibleTradeRaw = 0;
    }
    else if (_hiddenPolicy == HiddenOrderPolicy::InferIfTradeExceedsVisible)
    {
      // Compare reported trade volume against the last-known visible
      // level total on either side at this price. We do not know which
      // side the trade hit, so we check both and pick the smaller
      // cached visible (conservative: under-attribute to hidden).
      const int64_t cachedBid =
          lookupLevel({symbol, Side::BUY, price.raw()});
      const int64_t cachedAsk =
          lookupLevel({symbol, Side::SELL, price.raw()});
      const int64_t cachedVisible = (cachedBid > 0 && cachedAsk > 0)
                                        ? std::min(cachedBid, cachedAsk)
                                        : (cachedBid > 0 ? cachedBid : cachedAsk);
      if (cachedVisible > 0 && tradeQty.raw() > cachedVisible)
      {
        hiddenAttributedRaw = tradeQty.raw() - cachedVisible;
        visibleTradeRaw = cachedVisible;
      }
    }

    if (visibleTradeRaw > 0)
    {
      std::vector<std::pair<OrderId, Quantity>> filled;
      _tracker.onTrade(symbol, price, Quantity::fromRaw(visibleTradeRaw), filled);
      for (const auto& f : filled)
      {
        auto it = _orders.find(f.first);
        if (it != _orders.end())
        {
          it->second.lastUpdateNs = tsNs;
        }
      }
    }

    if (hiddenAttributedRaw > 0)
    {
      // Accumulate hidden volume on every tracked order at this price
      // on either side — we don't know which side the hidden order
      // sat on, so we credit it to all orders at the same price.
      for (auto& [oid, state] : _orders)
      {
        if (state.symbol == symbol && state.levelPriceRaw == price.raw())
        {
          state.hiddenVolumeSeenRaw += hiddenAttributedRaw;
        }
      }
    }

    touchOrdersAtPrice(symbol, price, tsNs);
  }

  int64_t lookupLevel(const LevelKey& k) const
  {
    auto it = _lastLevelQty.find(k);
    return it == _lastLevelQty.end() ? 0 : it->second;
  }

  OrderQueueTracker _tracker;
  std::unordered_map<OrderId, OrderState> _orders;
  int64_t _halfLifeNs{60'000'000'000LL};  // 60s default
  double _shrinkFactor{0.85};
  HiddenOrderPolicy _hiddenPolicy{HiddenOrderPolicy::Ignore};
  // Cache of last-known level totals keyed by (symbol, side, price).
  // Used by InferIfTradeExceedsVisible to decide how much of a trade
  // was hidden vs visible.
  std::unordered_map<LevelKey, int64_t, LevelKeyHash> _lastLevelQty;
};

}  // namespace flox
