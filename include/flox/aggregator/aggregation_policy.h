/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"
#include "flox/book/events/trade_event.h"

#include <algorithm>
#include <concepts>

namespace flox
{

template <typename T>
concept BarPolicy = requires(T& policy, const TradeEvent& trade, Bar& bar, TimePoint ts) {
  {
    policy.shouldClose(trade, bar)
  } noexcept -> std::same_as<bool>;
  {
    policy.update(trade, bar)
  } noexcept -> std::same_as<void>;
  {
    policy.initBar(trade, bar)
  } noexcept -> std::same_as<void>;
  {
    T::kBarType
  } -> std::convertible_to<BarType>;
  {
    policy.param()
  } noexcept -> std::convertible_to<uint64_t>;
};

// Common OHLCV update - free function, inlined
// Call this from policy.update() to update bar with trade data
inline void updateOHLCV(const TradeEvent& trade, Bar& bar) noexcept
{
  bar.high = std::max(bar.high, trade.trade.price);
  bar.low = std::min(bar.low, trade.trade.price);
  bar.close = trade.trade.price;

  // Volume = price * quantity (notional value)
  const auto notional = trade.trade.quantity * trade.trade.price;
  bar.volume += notional;

  bar.tradeCount += Quantity::fromRaw(1);

  if (trade.trade.isBuy)
  {
    bar.buyVolume += notional;
  }

  bar.endTime = fromUnixNs(trade.trade.exchangeTsNs);
}

// Initialize bar from first trade
inline void initBarFromTrade(const TradeEvent& trade, Bar& bar) noexcept
{
  const auto ts = fromUnixNs(trade.trade.exchangeTsNs);
  const auto notional = trade.trade.quantity * trade.trade.price;

  bar.open = trade.trade.price;
  bar.high = trade.trade.price;
  bar.low = trade.trade.price;
  bar.close = trade.trade.price;
  bar.volume = notional;
  bar.buyVolume = trade.trade.isBuy ? notional : Volume{};
  bar.tradeCount = Quantity::fromRaw(1);
  bar.startTime = ts;
  bar.endTime = ts;
  bar.reason = BarCloseReason::Threshold;
}

}  // namespace flox
