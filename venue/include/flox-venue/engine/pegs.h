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

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace flox::venue
{

// Pegged orders: the spec of each one, and the price it should be tracking.
//
// The engine keeps the loop -- a reprice cancels the order, re-reserves
// buying power and puts it back, which needs the book, the ledger and the
// sink. What is decided here is WHICH orders are repriced, in what order, and
// to what price.
class PegBook
{
 public:
  struct Peg
  {
    Side side;
    PegRef ref;
    int64_t offsetRaw;
  };

  // Everything the peg target is a function of, read off the book and the
  // instrument config by the caller. Passing it in rather than reaching for
  // the book is what makes the target a testable pure decision -- and the
  // caller has to take the order OUT of the book before it reads these, or a
  // peg that is the touch references itself.
  struct Market
  {
    bool hasBid{false};
    int64_t bidRaw{0};
    bool hasAsk{false};
    int64_t askRaw{0};
    int64_t lastRaw{0};      // reference of last resort when a side is missing
    int64_t tickRaw{0};      // 0 = no tick alignment
    int64_t minPriceRaw{0};  // 0 = no lower clamp (price band)
    int64_t maxPriceRaw{0};  // 0 = no upper clamp
  };

  // Peg price for `side` tracking `ref` (+ signed offset), tick-aligned,
  // clamped to not cross the opposite touch and to stay inside the price band.
  static int64_t targetRaw(Side side, PegRef ref, int64_t offsetRaw, const Market& m)
  {
    int64_t refRaw;
    switch (ref)
    {
      case PegRef::Bid:
        refRaw = m.hasBid ? m.bidRaw : (m.hasAsk ? m.askRaw : m.lastRaw);
        break;
      case PegRef::Ask:
        refRaw = m.hasAsk ? m.askRaw : (m.hasBid ? m.bidRaw : m.lastRaw);
        break;
      case PegRef::Mid:
        refRaw = (m.hasBid && m.hasAsk) ? (m.bidRaw + m.askRaw) / 2
                                        : (m.hasBid ? m.bidRaw : (m.hasAsk ? m.askRaw : m.lastRaw));
        break;
      default:
        return 0;
    }
    int64_t target = refRaw + offsetRaw;
    if (m.tickRaw > 0)
    {
      target = target / m.tickRaw * m.tickRaw;  // align down to tick
    }
    if (side == Side::BUY && m.hasAsk && target >= m.askRaw)
    {
      target = m.askRaw - m.tickRaw;  // never cross
    }
    if (side == Side::SELL && m.hasBid && target <= m.bidRaw)
    {
      target = m.bidRaw + m.tickRaw;
    }
    if (m.minPriceRaw > 0 && target < m.minPriceRaw)
    {
      target = m.minPriceRaw;
    }
    if (m.maxPriceRaw > 0 && target > m.maxPriceRaw)
    {
      target = m.maxPriceRaw;
    }
    return target;
  }

  void set(OrderId id, const Peg& p) { pegged_[id] = p; }

  void erase(OrderId id) { pegged_.erase(id); }

  const Peg* find(OrderId id) const
  {
    auto it = pegged_.find(id);
    return it == pegged_.end() ? nullptr : &it->second;
  }

  bool empty() const noexcept { return pegged_.empty(); }

  // The pegged ids, sorted. A peg reprice reads the book that PRIOR pegs in
  // the same pass already mutated, so the processing order is state-affecting
  // and must not depend on the table's layout -- the same
  // layout-independence standard as the ADL/liquidation paths.
  //
  // `ids` is cleared first: the caller reuses one buffer across passes.
  void sortedIds(std::vector<OrderId>& ids) const
  {
    ids.clear();
    ids.reserve(pegged_.size());
    // order: the pegged ids are sorted below -- a reprice reads the book
    // prior reprices in this pass already moved
    for (const auto& [id, p] : pegged_)
    {
      (void)p;
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
  }

  uint64_t hashInto(uint64_t h) const
  {
    for (OrderId id : sortedKeysOf(pegged_))
    {
      const Peg& p = pegged_.at(id);
      h = mix(h, 0xB003U);
      h = mix(h, id);
      h = mix(h, static_cast<uint64_t>(p.side));
      h = mix(h, static_cast<uint64_t>(p.ref));
      h = mix(h, static_cast<uint64_t>(p.offsetRaw));
    }
    return h;
  }

  void writeSnapshot(Journal& out, int64_t ts) const
  {
    for (OrderId id : sortedKeysOf(pegged_))
    {
      const Peg& p = pegged_.at(id);
      out.append(InboundCommand{RestorePeg{id, p.side, p.ref, {}, p.offsetRaw}}, ts);
    }
  }

 private:
  std::unordered_map<OrderId, Peg> pegged_;  // orderId -> peg spec (re-priced each submit)
};

}  // namespace flox::venue
