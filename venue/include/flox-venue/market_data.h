/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include "flox/book/book_update.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <unordered_map>
#include <utility>

namespace flox::venue
{

enum class MdType : uint8_t
{
  AddOrder,
  Trade,
  Executed,
  Cancel,
  Replace,
  Triggered,
};

struct MdMessage
{
  MdType type{};
  uint64_t seq{};
  SymbolId symbol{};
  OrderId id{};  // subject order; for Trade this is the taker
  Side side{};
  Price price{};
  Quantity qty{};     // AddOrder: shown qty; Trade: exec qty; Executed/Replace: leaves
  OrderId makerId{};  // Trade only
};

template <size_t Levels = (1u << 16)>
class MarketDataPublisher
{
 public:
  using MdSink = std::function<void(const MdMessage&)>;

  MarketDataPublisher(MdSink sink, Price tickSize, SymbolId symbol)
      : sink_(std::move(sink)), symbol_(symbol), book_(tickSize)
  {
  }

  void onEvent(const OutboundEvent& e)
  {
    if (const auto* x = std::get_if<OrderAccepted>(&e))
    {
      onAccepted(*x);
    }
    else if (const auto* x = std::get_if<Trade>(&e))
    {
      onTrade(*x);
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&e))
    {
      onExecuted(*x);
    }
    else if (const auto* x = std::get_if<OrderCanceled>(&e))
    {
      onCanceled(*x);
    }
    else if (const auto* x = std::get_if<OrderModified>(&e))
    {
      onModified(*x);
    }
    else if (const auto* x = std::get_if<OrderTriggered>(&e))
    {
      onTriggered(*x);
    }
    // OrderRejected: no market impact
  }

  // flox NLevelOrderBook: bestBid/bestAsk, bidAtPrice/askAtPrice, consumeAsks/
  // consumeBids (VWAP sweep), isCrossed, spread, mid.
  const flox::NLevelOrderBook<Levels>& book() const noexcept { return book_; }
  uint64_t seq() const noexcept { return seq_; }

  // Full book snapshot as AddOrder messages -- a late-joining subscriber applies
  // these, then resumes the incremental feed from `seq()`.
  std::vector<MdMessage> snapshot() const
  {
    std::vector<MdMessage> out;
    out.reserve(orders_.size());
    for (const auto& [id, r] : orders_)
    {
      out.push_back(MdMessage{MdType::AddOrder, 0, r.symbol, id, r.side, r.price, r.leaves, 0});
    }
    return out;
  }

 private:
  struct Resting
  {
    SymbolId symbol{};
    Side side{};
    Price price{};
    Quantity leaves{};
  };

  Quantity atPrice(Side s, Price p) const
  {
    return s == Side::BUY ? book_.bidAtPrice(p) : book_.askAtPrice(p);
  }

  // Apply a single absolute-qty level delta to the flox depth book.
  void applyLevel(Side s, Price p, Quantity newAggregate)
  {
    ev_.clear();
    ev_.update.type = flox::BookUpdateType::DELTA;
    ev_.update.symbol = symbol_;
    if (s == Side::BUY)
    {
      ev_.update.bids.emplace_back(p, newAggregate);
    }
    else
    {
      ev_.update.asks.emplace_back(p, newAggregate);
    }
    book_.applyBookUpdate(ev_);
  }

  void emit(MdMessage m)
  {
    m.seq = ++seq_;
    sink_(m);
  }

  void onAccepted(const OrderAccepted& a)
  {
    if (!a.restingOnBook)
    {
      return;  // pending stop: not on the visible book
    }
    // Public depth shows only the displayed size; an iceberg's hidden reserve is
    // never leaked (displayQty carries the peak, 0 -> non-iceberg, use leavesQty).
    const Quantity shown = a.displayQty.raw() > 0 ? a.displayQty : a.leavesQty;
    orders_[a.id] = Resting{a.symbol, a.side, a.price, shown};
    applyLevel(a.side, a.price, atPrice(a.side, a.price) + shown);
    emit(MdMessage{MdType::AddOrder, 0, a.symbol, a.id, a.side, a.price, shown, 0});
  }

  void onTrade(const Trade& t)
  {
    // Depth reduction is done via the maker's OrderExecuted; the print itself
    // does not touch depth (avoids double counting).
    emit(MdMessage{MdType::Trade, 0, t.symbol, t.takerId, t.takerSide, t.price, t.quantity,
                   t.makerId});
  }

  void onExecuted(const OrderExecuted& x)
  {
    auto it = orders_.find(x.id);
    if (it == orders_.end())
    {
      return;  // taker leg (not resting) -- ignore
    }
    Resting& r = it->second;
    // Use the DISPLAYED remaining, never the whole (peak+hidden) leavesQty -- else
    // an iceberg's hidden reserve leaks onto the public feed and a partial peak
    // fill (leavesQty > displayed) would fail to reduce depth / underflow the
    // level. r.leaves tracks the shown size; drive it from displayLeaves.
    const Quantity shownAfter = x.displayLeaves;
    const int64_t levelDelta = shownAfter.raw() - r.leaves.raw();
    if (levelDelta != 0)
    {
      applyLevel(r.side, r.price, Quantity::fromRaw(atPrice(r.side, r.price).raw() + levelDelta));
    }
    r.leaves = shownAfter;
    emit(MdMessage{MdType::Executed, 0, r.symbol, x.id, r.side, r.price, shownAfter, 0});
    if (x.complete)
    {
      orders_.erase(it);
    }
  }

  void onCanceled(const OrderCanceled& c)
  {
    auto it = orders_.find(c.id);
    if (it == orders_.end())
    {
      return;  // pending stop / unknown
    }
    const Resting r = it->second;
    applyLevel(r.side, r.price, atPrice(r.side, r.price) - r.leaves);
    orders_.erase(it);
    emit(MdMessage{MdType::Cancel, 0, r.symbol, c.id, r.side, r.price, r.leaves, 0});
  }

  void onModified(const OrderModified& m)
  {
    auto it = orders_.find(m.id);
    if (it == orders_.end())
    {
      return;
    }
    Resting& r = it->second;
    applyLevel(r.side, r.price, atPrice(r.side, r.price) - r.leaves);  // remove old
    r.price = m.price;
    r.leaves = m.leavesQty;
    applyLevel(r.side, r.price, atPrice(r.side, r.price) + m.leavesQty);  // add new
    emit(MdMessage{MdType::Replace, 0, r.symbol, m.id, r.side, m.price, m.leavesQty, 0});
  }

  void onTriggered(const OrderTriggered& t)
  {
    emit(MdMessage{MdType::Triggered, 0, t.symbol, t.id, Side::BUY, t.refPrice, Quantity{}, 0});
  }

  MdSink sink_;
  SymbolId symbol_{};
  flox::NLevelOrderBook<Levels> book_;
  std::pmr::memory_resource* res_ = std::pmr::new_delete_resource();
  flox::BookUpdateEvent ev_{res_};
  std::unordered_map<OrderId, Resting> orders_;
  uint64_t seq_{0};
};

}  // namespace flox::venue
