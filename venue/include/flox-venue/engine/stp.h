/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/engine/sorted_keys.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/messages.h"

#include <cstdint>
#include <unordered_map>

namespace flox::venue
{

// Self-trade prevention, engine side: the mode each resting order was admitted
// with, the scope two accounts are compared in, and the verdict an auction
// uncross reaches on a self-matching pair.
//
// Continuous matching takes the mode off the aggressor and decides inside the
// matcher; what is here is the part the matcher cannot see -- an auction has
// no aggressor, both legs are resting, and the mode has to be read back off
// each of them. The table is sparse: STPMode::None is absent.
class StpState
{
 public:
  // Mode recorded for an order, or None if it asked for none. Reading it back
  // is how a re-entering order keeps the control it was admitted with.
  STPMode modeOf(OrderId id) const
  {
    auto it = modes_.find(id);
    return it == modes_.end() ? STPMode::None : it->second;
  }

  // Every path that puts an order on the book says what self-trade prevention
  // it carries. On the hot path of every rest -- non-virtual and defined here
  // so it inlines exactly as the member function it replaced did.
  void track(OrderId id, STPMode stp)
  {
    if (stp != STPMode::None)
    {
      modes_[id] = stp;
    }
    else
    {
      modes_.erase(id);  // an id can be reused after the previous order left
    }
  }

  void forget(OrderId id) { modes_.erase(id); }

  // Recovery: a RestoreOrderStp record. None is not stored, so a snapshot that
  // carries one leaves the table as it was.
  void restore(OrderId id, STPMode mode)
  {
    if (mode != STPMode::None)
    {
      modes_[id] = mode;
    }
  }

  uint64_t hashInto(uint64_t h) const
  {
    for (OrderId id : sortedKeysOf(modes_))
    {
      h = mix(h, 0xB00CU);
      h = mix(h, static_cast<uint64_t>(id));
      h = mix(h, static_cast<uint64_t>(modes_.at(id)));
    }
    return h;
  }

  void writeSnapshot(Journal& out, int64_t ts) const
  {
    for (OrderId id : sortedKeysOf(modes_))
    {
      out.append(InboundCommand{RestoreOrderStp{id, static_cast<uint8_t>(modes_.at(id))}}, ts);
    }
  }

  // ---- checkpoint: the firm-group table ---------------------------------
  // The groups live in the matcher (they feed matching decisions) and are
  // journaled as SetStpGroup commands, but this component is already the one
  // that reads them -- see scopeOf below -- so the canonical traversal is
  // here rather than a second copy of it in the engine. Read through, never
  // copied. An empty table writes and folds nothing.
  uint64_t hashGroupsInto(const std::unordered_map<uint64_t, uint64_t>& groups, uint64_t h) const
  {
    for (uint64_t acct : sortedKeysOf(groups))
    {
      h = mix(h, 0xB00BU);
      h = mix(h, acct);
      h = mix(h, groups.at(acct));
    }
    return h;
  }

  // Re-emitted as the command that set them, applied through the ordinary
  // submit path on load (config section).
  void writeGroups(const std::unordered_map<uint64_t, uint64_t>& groups, SymbolId symbol,
                   Journal& out, int64_t ts) const
  {
    for (uint64_t acct : sortedKeysOf(groups))
    {
      out.append(InboundCommand{SetStpGroup{symbol, {}, acct, groups.at(acct)}}, ts);
    }
  }

  // The scope two accounts are compared in: the firm group if the account is
  // in one, else the account itself. `groups` is the matcher's own table, read
  // through rather than copied so the two can never disagree about who counts
  // as the same trader.
  static uint64_t scopeOf(const std::unordered_map<uint64_t, uint64_t>& groups, uint64_t account)
  {
    if (groups.empty())
    {
      return account;
    }
    auto it = groups.find(account);
    return it == groups.end() ? account : it->second;
  }

  // What an auction uncross must do with a pair of self-matching legs.
  // `engaged` false means neither leg asked for prevention and the pair
  // prints normally.
  struct AuctionVerdict
  {
    bool engaged{false};
    bool decrement{false};
    bool cancelBid{false};
    bool cancelAsk{false};
  };

  // An auction has no aggressor -- both legs are resting -- so the mode is
  // read off each order and applied from the requester's point of view: its
  // counterparty is the "oldest" leg (it is resting) and its own order is the
  // "newest". When both legs ask, the cancellations union, which needs no
  // precedence rule between modes and lands the same way whichever order the
  // book hands them to us in. Decrement outranks the cancels for the same
  // reason: either leg asking for it trims both and the pair never prints.
  static AuctionVerdict auctionVerdict(STPMode bidStp, STPMode askStp)
  {
    AuctionVerdict v;
    if (bidStp == STPMode::None && askStp == STPMode::None)
    {
      return v;
    }
    v.engaged = true;
    if (bidStp == STPMode::Decrement || askStp == STPMode::Decrement)
    {
      v.decrement = true;
      return v;
    }
    if (bidStp == STPMode::CancelOldest || bidStp == STPMode::CancelBoth)
    {
      v.cancelAsk = true;
    }
    if (bidStp == STPMode::CancelNewest || bidStp == STPMode::CancelBoth)
    {
      v.cancelBid = true;
    }
    if (askStp == STPMode::CancelOldest || askStp == STPMode::CancelBoth)
    {
      v.cancelBid = true;
    }
    if (askStp == STPMode::CancelNewest || askStp == STPMode::CancelBoth)
    {
      v.cancelAsk = true;
    }
    return v;
  }

 private:
  std::unordered_map<OrderId, STPMode> modes_;
};

}  // namespace flox::venue
