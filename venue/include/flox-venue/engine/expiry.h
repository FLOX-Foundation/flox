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
#include <vector>

namespace flox::venue
{

// GTD deadlines: orderId -> expiry, in sequencer time.
//
// A good-till-date order that never reaches this table never expires at all,
// so every path that admits one registers it here -- resting orders and
// conditional ones alike (a stop whose deadline passes before it ever
// triggers has to expire, not wait forever for a price that may never come).
//
// The component owns WHICH orders are due and in what order; cancelling them
// is the engine's, because that is where the book and the reports are.
class ExpiryBook
{
 public:
  void set(OrderId id, SeqNanos expiry) { expiry_[id] = expiry; }

  void erase(OrderId id) { expiry_.erase(id); }

  // The deadline of an order, or a zero SeqNanos when it carries none. Folded
  // into the state hash and written into RestoreOrder, so "absent" and "zero"
  // are deliberately the same value.
  SeqNanos expiryOf(OrderId id) const
  {
    auto it = expiry_.find(id);
    return it == expiry_.end() ? SeqNanos{} : it->second;
  }

  bool empty() const noexcept { return expiry_.empty(); }

  // The orders whose deadline has passed at `now`, id-sorted.
  //
  // The comparison is `now >= deadline`, inclusive: an order good until T is
  // gone AT T, not one tick after it. The sort is what makes the expiry and
  // the events it produces deterministic -- the table is an unordered_map, so
  // its enumeration order is a property of the standard library rather than of
  // the state.
  //
  // `due` is cleared first: the caller reuses one buffer across sweeps.
  void collectDue(SeqNanos now, std::vector<OrderId>& due) const
  {
    due.clear();
    if (expiry_.empty())
    {
      return;
    }
    // order: the due orders are id-sorted below, before any expiry is
    // published
    for (const auto& [id, exp] : expiry_)
    {
      if (now >= exp)
      {
        due.push_back(id);
      }
    }
    std::sort(due.begin(), due.end());  // deterministic expiry/event order (layout-independent)
  }

 private:
  std::unordered_map<OrderId, SeqNanos> expiry_;
};

}  // namespace flox::venue
