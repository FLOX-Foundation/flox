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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <variant>
#include <vector>

namespace flox::venue
{

// Tracks the session's live orders so a disconnect can pull them.
//
// Memory is bounded by pruning: untrack(id) drops an order once a terminal
// exec report (complete fill / cancel / reject) is observed for it -- the
// gateway wires this through the per-session delivery observer. Without
// pruning this grew one entry per NewOrder forever and the disconnect flush
// sent cancels for orders that had been gone for hours.
//
// Thread-safe: track/flush run on the connection reader thread, untrack is
// called from the delivery (matching-side) path.
class DisconnectCanceller
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  // Third argument: steady-clock receipt time of the frame that carried the
  // command, 0 when there is no frame (synthetic cancels from flush). The
  // stamp exists so a shard can decompose latency; a handler that does not
  // care simply ignores it.
  using Handler = std::function<void(const InboundCommand&, const Responder&, int64_t)>;

  explicit DisconnectCanceller(bool enabled) noexcept : enabled_(enabled) {}

  // Wire-negotiated cancel-on-disconnect (FIX Logon tag 20003 / SBE
  // SetSessionConfig): flips the live flag. Negotiation happens at logon /
  // before order flow, so there is no tracked backlog to reconcile; orders
  // placed while disabled are simply never tracked.
  void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }

  // Record a command placed on this session (only NewOrders are cancellable).
  void track(const InboundCommand& cmd)
  {
    if (!enabled())
    {
      return;
    }
    if (const auto* n = std::get_if<NewOrder>(&cmd))
    {
      std::lock_guard<std::mutex> lk(m_);
      placed_[n->id] = CancelOrder{n->id, n->symbol, n->accountId};
    }
  }

  // Terminal exec report seen for `id`: no cancel needed on disconnect.
  void untrack(OrderId id)
  {
    if (!enabled())
    {
      return;
    }
    std::lock_guard<std::mutex> lk(m_);
    placed_.erase(id);
  }

  // Observe a routed outbound event and prune terminally-resolved orders.
  void observe(const OutboundEvent& e)
  {
    if (!enabled())
    {
      return;
    }
    if (const auto* x = std::get_if<OrderExecuted>(&e))
    {
      if (x->complete)
      {
        untrack(x->id);
      }
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&e))
    {
      untrack(c->id);
    }
    else if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      untrack(r->id);
    }
    // CancelRejected is deliberately NOT untracked: a refused cancel leaves the
    // order resting, and forgetting it here would leave an order this is
    // supposed to pull on disconnect.
  }

  // On disconnect, cancel everything this session still has live.
  void flush(const Handler& handler)
  {
    if (!enabled())
    {
      return;
    }
    std::unordered_map<OrderId, CancelOrder> live;
    {
      std::lock_guard<std::mutex> lk(m_);
      live.swap(placed_);
    }
    // Deterministic cancel order (id-sorted), independent of map layout.
    std::vector<CancelOrder> ordered;
    ordered.reserve(live.size());
    for (const auto& [id, c] : live)
    {
      (void)id;
      ordered.push_back(c);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const CancelOrder& a, const CancelOrder& b)
              { return a.id < b.id; });
    const Responder noop = [](const uint8_t*, size_t) {};
    for (const auto& c : ordered)
    {
      handler(InboundCommand{c}, noop, 0);
    }
  }

  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  size_t tracked() const
  {
    std::lock_guard<std::mutex> lk(m_);
    return placed_.size();
  }

 private:
  std::atomic<bool> enabled_;
  mutable std::mutex m_;
  std::unordered_map<OrderId, CancelOrder> placed_;
};

}  // namespace flox::venue
