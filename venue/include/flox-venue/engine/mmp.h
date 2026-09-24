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
#include "flox-venue/engine/state_hash_tags.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/messages.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

// Market-maker protection: if an account is filled for more than its limit
// inside its window, all of its resting orders are pulled.
//
// The component counts the fills and decides whose quotes have to go; pulling
// them is the engine's, because that is where the book and the reports are.
// The breach list is collected during matching and drained after it, so the
// book is never mutated mid-match.
class MmpState
{
 public:
  // Whether any account has MMP configured. The trade path asks first: the
  // common case is no MMP at all, and a per-trade hash lookup for a feature
  // nobody switched on is pure cost.
  bool armed() const noexcept { return !cfg_.empty(); }

  void configure(uint64_t account, Quantity qtyLimit, DurationNs windowNs)
  {
    cfg_[account] = Cfg{qtyLimit, windowNs};
  }

  // One observed fill. The window carries an incrementally maintained sum, so
  // a breach check is O(1) amortised rather than an O(n) rescan of the deque
  // on every fill (an active MM inside the window would otherwise be O(n^2)).
  void add(uint64_t account, Quantity qty, SeqNanos now)
  {
    auto cfg = cfg_.find(account);
    if (cfg == cfg_.end())
    {
      return;
    }
    Window& w = fills_[account];
    w.fills.emplace_back(now, qty);
    w.sumRaw += qty.raw();
    while (!w.fills.empty() && w.fills.front().first <= now - cfg->second.windowNs)
    {
      w.sumRaw -= w.fills.front().second.raw();
      w.fills.pop_front();
    }
    if (w.sumRaw >= cfg->second.qtyLimit.raw())  // sum >= limit
    {
      bool queued = false;
      for (uint64_t a : breached_)
      {
        queued |= (a == account);
      }
      if (!queued)
      {
        breached_.push_back(account);
      }
    }
  }

  // Accounts whose quotes are due to be pulled, in the order their breach was
  // first observed.
  const std::vector<uint64_t>& breached() const noexcept { return breached_; }

  // Re-arm after the pull: drop the window and its running sum, so the maker
  // starts the next window from nothing rather than from fills it has already
  // been punished for.
  void rearm(uint64_t account)
  {
    Window& w = fills_[account];
    w.fills.clear();
    w.sumRaw = 0;
  }

  void clearBreached() { breached_.clear(); }

  // Recovery: config for one account.
  void restoreCfg(uint64_t account, Quantity qtyLimit, DurationNs windowNs)
  {
    // Fill windows arrive separately as RestoreMmpFills records (exact
    // restore); a snapshot without them restores the windows empty.
    cfg_[account] = Cfg{qtyLimit, windowNs};
  }

  // Recovery: one batch of window fills. False when the record is malformed,
  // which proves the snapshot corrupt.
  bool restoreFills(const RestoreMmpFills& r)
  {
    if (r.count > kMmpFillBatch)
    {
      return false;
    }
    Window& w = fills_[r.account];
    for (uint32_t i = 0; i < r.count; ++i)
    {
      w.fills.emplace_back(SeqNanos::fromRaw(r.tsNs[i]), Quantity::fromRaw(r.qtyRaw[i]));
      w.sumRaw += r.qtyRaw[i];
    }
    return true;
  }

  uint64_t hashInto(uint64_t h) const
  {
    for (uint64_t acct : sortedKeysOf(cfg_))
    {
      const Cfg& c = cfg_.at(acct);
      h = mix(h, hash_tags::kMmpConfig);
      h = mix(h, acct);
      h = mix(h, static_cast<uint64_t>(c.qtyLimit.raw()));
      h = mix(h, static_cast<uint64_t>(c.windowNs.count()));
    }

    // Sliding-window fills, deque (time) order. An EMPTY window contributes
    // nothing, so it is indistinguishable from absence -- which also keeps
    // pre-window-serialization snapshots (that restored windows empty)
    // hashing identically when the windows really were empty.
    for (uint64_t acct : sortedKeysOf(fills_))
    {
      const Window& w = fills_.at(acct);
      if (w.fills.empty())
      {
        continue;
      }
      h = mix(h, hash_tags::kMmpFills);
      h = mix(h, acct);
      for (const auto& [ts, q] : w.fills)
      {
        h = mix(h, static_cast<uint64_t>(ts.raw()));
        h = mix(h, static_cast<uint64_t>(q.raw()));
      }
    }
    return h;
  }

  void writeSnapshot(Journal& out, int64_t ts) const
  {
    for (uint64_t acct : sortedKeysOf(cfg_))
    {
      const Cfg& c = cfg_.at(acct);
      out.append(InboundCommand{RestoreMmpCfg{acct, c.qtyLimit, c.windowNs}}, ts);
    }

    // Window fills, exact, in deque (time) order -- a maker one fill from its
    // limit stays one fill from it across recovery.
    for (uint64_t acct : sortedKeysOf(fills_))
    {
      const Window& w = fills_.at(acct);
      if (w.fills.empty())
      {
        continue;
      }
      RestoreMmpFills batch{};
      batch.account = acct;
      for (const auto& [fts, q] : w.fills)
      {
        batch.tsNs[batch.count] = fts.raw();  // wire batch: raw ticks
        batch.qtyRaw[batch.count] = q.raw();
        if (++batch.count == kMmpFillBatch)
        {
          out.append(InboundCommand{batch}, ts);
          batch = RestoreMmpFills{};
          batch.account = acct;
        }
      }
      if (batch.count > 0)
      {
        out.append(InboundCommand{batch}, ts);
      }
    }
  }

 private:
  struct Cfg
  {
    Quantity qtyLimit{};
    DurationNs windowNs{};
  };

  struct Window
  {
    std::deque<std::pair<SeqNanos, Quantity>> fills;
    int64_t sumRaw{0};  // running sum of fills.second.raw()
  };

  std::unordered_map<uint64_t, Cfg> cfg_;

  std::unordered_map<uint64_t, Window> fills_;

  std::vector<uint64_t> breached_;
};

}  // namespace flox::venue
