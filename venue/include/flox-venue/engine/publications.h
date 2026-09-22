/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/event_sink.h"
#include "flox-venue/messages.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flox::venue::engine
{

// What turns the engine's internal facts into the derivatives and cancel
// events a subscriber reads, and the per-account index the cancel-everything
// paths walk.
//
// Those two read like separate jobs and are not. An order this index stops
// tracking is an order no mass cancel can name again, and every cancel the
// index produces leaves through the same sink the derivatives publication
// leaves through -- one place that decides what those outbound events look
// like and in what order a set of them goes out.
//
// The trading-status publication is NOT here: the status, its transitions
// and the memo of what was last published belong to the state they report on
// and live next to it, in engine::Session (flox-venue/engine/session.h).
//
// The book is NOT here, deliberately. Which orders exist, and what removing
// one does to reservations, positions and holds, stays with the engine. This
// component answers a narrower question: which ids does an account hold, in
// what order are they handed back, and what do the resulting events look like.
// A mass cancel therefore keeps its loop in the engine and takes only the
// id list from here.
//
// The sink is held BY REFERENCE and called directly. There is no virtual on
// the publication path and there must not be one: the engine publishes on
// every trade, and a vtable dispatch there is a cost with nothing bought by
// it. The reference is to a sink the engine owns for the engine's lifetime.
class Publications
{
 public:
  Publications(const EventSink& sink, SymbolId symbol) noexcept : sink_(sink), symbol_(symbol) {}

  // ---- instrument-wide publications -------------------------------------

  // The derivatives layer as the engine knows it: the mark it was just given,
  // the last funding rate it applied, the next funding boundary of the
  // configured schedule and the live open interest.
  //
  // Formatting only. WHEN this goes out is clearing's decision -- it is the
  // side that knows a mark or a funding application just landed.
  void publishDerivatives(Price mark, int64_t fundingRateRaw, SeqNanos nextFundingNs,
                          Quantity openInterest) const
  {
    sink_(DerivativesUpdated{symbol_, mark, fundingRateRaw, nextFundingNs, openInterest});
  }

  // One cancel report. Every engine-side cancel names the order the same way:
  // the venue's id, the owner it was routed for, and the identifier the
  // submitter itself chose. leavesQty/cumQty (T058) default to 0 for a
  // caller with nothing better -- correct for a pending stop or an order
  // already gone by the time this runs, wrong for a resting order a caller
  // still holds; those callers pass the real RestingOrder-derived values.
  void publishCanceled(OrderId id, CancelReason reason, uint64_t account, uint64_t clientOrderId,
                       Quantity leavesQty = {}, Quantity cumQty = {}) const
  {
    sink_(OrderCanceled{id, symbol_, reason, account, clientOrderId, leavesQty, cumQty});
  }

  // ---- per-account resting-order tracking -------------------------------

  // Every path that puts an order on the book comes through here.
  void trackResting(OrderId id, uint64_t account)
  {
    orderAccount_[id] = account;
    byAccount_[account].insert(id);
  }

  // Drop an order from the index. Returns whether it was tracked at all, so
  // the engine's own per-order cleanup runs on exactly the ids this forgot --
  // an untracked id owns nothing here and owns nothing there either.
  bool forget(OrderId id)
  {
    auto it = orderAccount_.find(id);
    if (it == orderAccount_.end())
    {
      return false;
    }
    auto ba = byAccount_.find(it->second);
    if (ba != byAccount_.end())
    {
      ba->second.erase(id);
    }
    orderAccount_.erase(it);
    return true;
  }

  // Owner of a live tracked order (0 if unknown) -- read BEFORE forget so
  // async cancel events can be routed to the owner's session.
  uint64_t ownerOf(OrderId id) const noexcept
  {
    auto it = orderAccount_.find(id);
    return it == orderAccount_.end() ? 0 : it->second;
  }

  // Owner, or nullptr when the id is not tracked. Separate from ownerOf
  // because an ownership check must tell "no such order" apart from "owned by
  // account 0", and a 0 return cannot.
  const uint64_t* trackedOwner(OrderId id) const noexcept
  {
    auto it = orderAccount_.find(id);
    return it == orderAccount_.end() ? nullptr : &it->second;
  }

  bool tracked(OrderId id) const noexcept { return orderAccount_.count(id) != 0; }

  // Live resting orders tracked on this symbol (observability gauge).
  uint64_t restingOrderCount() const noexcept { return orderAccount_.size(); }

  // How many the account holds; 0 for an account with none, so a cap check
  // needs no separate "is it there" test.
  size_t accountOrderCount(uint64_t account) const noexcept
  {
    auto it = byAccount_.find(account);
    return it == byAccount_.end() ? 0 : it->second.size();
  }

  // The account's ids, or nullptr when it holds none. Handed out for folds
  // that do not publish -- a caller that PUBLISHES per id takes cancelOrderFor
  // instead, which is sorted.
  const std::unordered_set<OrderId>* ordersOf(uint64_t account) const noexcept
  {
    auto it = byAccount_.find(account);
    return it == byAccount_.end() ? nullptr : &it->second;
  }

  // The ids of one account, in the order a mass cancel must walk them. Sorted
  // rather than enumerated: each id publishes an OrderCanceled, so bucket
  // order would make the EVENT SEQUENCE a client receives depend on which
  // standard library the venue was built against while the set of cancels is
  // identical either way.
  std::vector<OrderId> cancelOrderFor(uint64_t account) const
  {
    std::vector<OrderId> ids;
    auto it = byAccount_.find(account);
    if (it == byAccount_.end())
    {
      return ids;
    }
    ids.reserve(it->second.size());
    // order: collected here, id-sorted below -- each id publishes a cancel
    for (OrderId id : it->second)
    {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());  // deterministic cancel/event order (layout-independent)
    return ids;
  }

  // Every tracked id, same discipline and for the same reason: an
  // instrument-wide sweep publishes one cancel per surviving id.
  std::vector<OrderId> cancelOrderAll() const
  {
    std::vector<OrderId> ids;
    ids.reserve(orderAccount_.size());
    // order: collected here, id-sorted below -- each surviving id publishes
    // an OrderCanceled
    for (const auto& [acct, own] : byAccount_)
    {
      (void)acct;
      ids.insert(ids.end(), own.begin(), own.end());
    }
    std::sort(ids.begin(), ids.end());  // deterministic cancel order (layout-independent)
    return ids;
  }

  // The async-checkpoint clone takes the tracking state; it does NOT take the
  // sink, which is the clone's own (a null sink -- a clone must not republish
  // what the live engine already published).
  //
  // Nothing here is hashed or snapshotted: the tracking maps are an index
  // over the book, which is hashed already. STP moved to engine::StpState
  // (venue/include/flox-venue/engine/stp.h) and the trading-status memo to
  // engine::Session (engine/session.h); each owns its own checkpoint.
  void copyStateFrom(const Publications& other)
  {
    orderAccount_ = other.orderAccount_;
    byAccount_ = other.byAccount_;
  }

 private:
  const EventSink& sink_;

  SymbolId symbol_{};

  std::unordered_map<OrderId, uint64_t> orderAccount_;

  std::unordered_map<uint64_t, std::unordered_set<OrderId>> byAccount_;
};

}  // namespace flox::venue::engine
