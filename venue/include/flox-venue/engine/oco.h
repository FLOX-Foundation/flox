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

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue::engine
{

// One-cancels-the-other: which orders belong to which group, which group a
// fill just decided, and which members lose because of it.
//
// The membership is kept twice on purpose -- order -> group answers the
// question the fill path asks on every print, group -> members answers the
// one the resolution asks once -- and the two indexes are only correct
// together. Every departure routes through unlink() for exactly that reason:
// a leg that leaves the venue by any door (cancel, expiry, MMP, halt,
// liquidation, reject) but stays in members_ will later cancel whatever order
// REUSES its id, and the group vector leaks besides.
//
// The component owns membership and the verdict. Cancelling the losers is the
// engine's, because that is where the book, the reservations and the reports
// are. Nothing here reaches any of the three.
//
// Group 0 is "no group": it is the absent value, never a real group id.
class OcoBook
{
 public:
  // Linked BEFORE matching, so a fill on the incoming order itself decides
  // its group too, not only a fill on something already resting.
  void link(OrderId id, uint64_t group)
  {
    group_[id] = group;
    members_[group].push_back(id);
  }

  // Remove one order from its group, keeping both indexes in step. A no-op
  // for an order that is in no group, so every exit path can call it blindly
  // -- and almost every one of them does, on every order that ever leaves the
  // venue. Hence the shape: the "nothing is grouped at all" answer is a load
  // and a branch that inlines into the caller, and the hash lookup is behind
  // a call that the common case never makes.
  void unlink(OrderId id)
  {
    if (group_.empty())
    {
      return;
    }
    unlinkGrouped(id);
  }

  // The group an order belongs to, or 0. Folded into the state hash and
  // written into RestoreOrder / RestoreStop, so "absent" and "group 0" are
  // deliberately the same value.
  uint64_t groupOf(OrderId id) const
  {
    auto it = group_.find(id);
    return it == group_.end() ? 0 : it->second;
  }

  // True when no order is grouped at all -- the common case, and the reason
  // the per-trade lookups below can be skipped entirely on the hot path.
  bool empty() const noexcept { return group_.empty(); }

  // A print: either side of it may be a grouped order, and either wins its
  // group. Collected rather than resolved, because resolving means cancelling
  // and the book must not be mutated in the middle of a match.
  void noteFill(OrderId makerId, OrderId takerId)
  {
    if (auto it = group_.find(makerId); it != group_.end())
    {
      pending_.emplace_back(it->second, makerId);
    }
    if (auto it = group_.find(takerId); it != group_.end())
    {
      pending_.emplace_back(it->second, takerId);
    }
  }

  bool nothingPending() const noexcept { return pending_.empty(); }

  // The orders that lost, in the order they are to be cancelled, and the
  // groups they were in are dissolved as they are handed over. A group that
  // two prints decided in the same submit is dissolved by the first and
  // skipped by the second.
  //
  // Siblings come out id-sorted: group membership is a SET (the insertion
  // order is not state -- a checkpoint restore rebuilds it in canonical book
  // order), so neither the cancels nor the events they produce may depend on
  // it. `out` is cleared first; the caller reuses one buffer.
  void drainLosers(std::vector<OrderId>& out)
  {
    out.clear();
    for (const auto& [group, winner] : pending_)
    {
      auto git = members_.find(group);
      if (git == members_.end())
      {
        continue;  // already resolved this submit
      }
      std::vector<OrderId> members = git->second;
      // order: id-sorted below, before any sibling is handed back
      std::sort(members.begin(), members.end());
      members_.erase(git);
      for (OrderId id : members)
      {
        group_.erase(id);
        if (id != winner)
        {
          out.push_back(id);
        }
      }
    }
    pending_.clear();
  }

 private:
  void unlinkGrouped(OrderId id)
  {
    auto it = group_.find(id);
    if (it == group_.end())
    {
      return;
    }
    if (auto gm = members_.find(it->second); gm != members_.end())
    {
      auto& v = gm->second;
      v.erase(std::remove(v.begin(), v.end(), id), v.end());
      if (v.empty())
      {
        members_.erase(gm);
      }
    }
    group_.erase(it);
  }

  std::unordered_map<OrderId, uint64_t> group_;                 // orderId -> OCO group
  std::unordered_map<uint64_t, std::vector<OrderId>> members_;  // group -> member orderIds
  std::vector<std::pair<uint64_t, OrderId>> pending_;           // (group, winner) collected while matching
};

}  // namespace flox::venue::engine
