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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

namespace flox::venue
{

class DisconnectCanceller
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  using Handler = std::function<void(const InboundCommand&, const Responder&)>;

  explicit DisconnectCanceller(bool enabled) noexcept : enabled_(enabled) {}

  // Record a command placed on this session (only NewOrders are cancellable).
  void track(const InboundCommand& cmd)
  {
    if (!enabled_)
    {
      return;
    }
    if (const auto* n = std::get_if<NewOrder>(&cmd))
    {
      placed_.push_back(CancelOrder{n->id, n->symbol, n->accountId});
    }
  }

  // On disconnect, cancel everything this session left resting.
  void flush(const Handler& handler)
  {
    if (!enabled_)
    {
      return;
    }
    const Responder noop = [](const uint8_t*, size_t) {};
    for (const auto& c : placed_)
    {
      handler(InboundCommand{c}, noop);
    }
    placed_.clear();
  }

  bool enabled() const noexcept { return enabled_; }

 private:
  bool enabled_;
  std::vector<CancelOrder> placed_;
};

}  // namespace flox::venue
