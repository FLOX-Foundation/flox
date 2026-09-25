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

// Optional policy customization points. A policy that does not declare them
// keeps the plain close path (emit the bar as it stands, open the next one
// from the trade); the two aggregators and the batch aggregators in the C
// ABI and the Python bindings all branch on these concepts, so a policy
// gains the behaviour in every copy of the close path at once instead of in
// whichever one was remembered.

// Sink handed to closeAndReopen(). Only used to state the requirement; each
// aggregator passes its own publishing lambda.
struct BarSink
{
  void operator()(const Bar&) const noexcept {}
};

// The policy owns its whole close step: it finishes the bar that was
// forming, hands every bar to publish to the sink in order, and leaves `bar`
// re-initialized as the one that opens next. Renko needs this because a
// brick closes at a price boundary rather than wherever the crossing trade
// landed, so the close price and the next bar's open are the same number and
// have to be computed together. See RenkoBarPolicy::closeAndReopen.
template <typename T>
concept ClosesAndReopens = requires(const T& policy, const TradeEvent& trade, Bar& bar, BarSink sink) {
  policy.closeAndReopen(trade, bar, sink);
};

// The policy can recognize a trade that belongs to a bar that is already
// gone. Such a trade is dropped rather than folded into the live bar; only
// TimeBarPolicy has a notion of a bar the clock has moved past.
template <typename T>
concept DetectsLateTrades = requires(const T& policy, const TradeEvent& trade, const Bar& bar) {
  {
    policy.isLate(trade, bar)
  } noexcept -> std::same_as<bool>;
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
