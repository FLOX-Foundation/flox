/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/aggregation_policy.h"
#include "flox/strategy/symbol_state_map.h"

#include <chrono>

namespace flox
{

/// Heikin-Ashi bar policy
/// Produces smoothed bars using Heikin-Ashi formula:
/// - HA_Close = (O + H + L + C) / 4
/// - HA_Open = (prev_HA_Open + prev_HA_Close) / 2
/// - HA_High = max(H, HA_Open, HA_Close)
/// - HA_Low = min(L, HA_Open, HA_Close)
///
/// Uses time-based intervals like TimeBarPolicy.
/// Supports multi-symbol with per-symbol state tracking.
class HeikinAshiBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::HeikinAshi;

  template <typename Rep, typename Period>
  explicit constexpr HeikinAshiBarPolicy(std::chrono::duration<Rep, Period> interval) noexcept
      : _interval(std::chrono::duration_cast<std::chrono::nanoseconds>(interval))
  {
  }

  /// Returns interval in nanoseconds
  constexpr uint64_t param() const noexcept { return _interval.count(); }

  [[nodiscard]] bool shouldClose(const TradeEvent& trade, const Bar& bar) const noexcept
  {
    const auto tradeTs = fromUnixNs(trade.trade.exchangeTsNs);
    const auto alignedTradeTs = alignToInterval(tradeTs);
    return alignedTradeTs != bar.startTime;
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    auto& state = _symbolState[trade.trade.symbol];

    // Update raw OHLC
    state.rawHigh = std::max(state.rawHigh, trade.trade.price);
    state.rawLow = std::min(state.rawLow, trade.trade.price);
    state.rawClose = trade.trade.price;

    // Update volume/trade count in bar
    const auto notional = trade.trade.quantity * trade.trade.price;
    bar.volume += notional;
    bar.tradeCount += Quantity::fromRaw(1);

    if (trade.trade.isBuy)
    {
      bar.buyVolume += notional;
    }

    // Apply Heikin-Ashi transformation
    applyHeikinAshi(state, bar);
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    auto& state = _symbolState[trade.trade.symbol];

    // Save current bar's HA values for next bar calculation BEFORE resetting
    // (if bar has valid data from previous session)
    if (bar.open.raw() != 0)
    {
      state.prevHaOpen = bar.open;
      state.prevHaClose = bar.close;
    }

    const auto ts = fromUnixNs(trade.trade.exchangeTsNs);
    const auto notional = trade.trade.quantity * trade.trade.price;

    // Store raw OHLC for this symbol
    state.rawOpen = trade.trade.price;
    state.rawHigh = trade.trade.price;
    state.rawLow = trade.trade.price;
    state.rawClose = trade.trade.price;

    // Initialize bar with raw values first
    bar.open = trade.trade.price;
    bar.high = trade.trade.price;
    bar.low = trade.trade.price;
    bar.close = trade.trade.price;
    bar.volume = notional;
    bar.buyVolume = trade.trade.isBuy ? notional : Volume{};
    bar.tradeCount = Quantity::fromRaw(1);
    bar.startTime = alignToInterval(ts);
    bar.endTime = bar.startTime + _interval;
    bar.reason = BarCloseReason::Threshold;

    // Apply Heikin-Ashi transformation
    applyHeikinAshi(state, bar);
  }

 private:
  /// Per-symbol state for Heikin-Ashi calculation
  struct SymbolHAState
  {
    // Raw OHLC for current bar (before HA transformation)
    Price rawOpen{};
    Price rawHigh{};
    Price rawLow{};
    Price rawClose{};

    // Previous bar's HA values for calculating current HA_Open
    Price prevHaOpen{};
    Price prevHaClose{};
  };

  void applyHeikinAshi(const SymbolHAState& state, Bar& bar) const noexcept
  {
    // HA_Close = (O + H + L + C) / 4
    const auto haClose = Price::fromRaw(
        (state.rawOpen.raw() + state.rawHigh.raw() + state.rawLow.raw() + state.rawClose.raw()) /
        4);

    // HA_Open = (prev_HA_Open + prev_HA_Close) / 2
    // For first bar, use (rawOpen + rawClose) / 2
    Price haOpen;
    if (state.prevHaOpen.raw() == 0 && state.prevHaClose.raw() == 0)
    {
      haOpen = Price::fromRaw((state.rawOpen.raw() + state.rawClose.raw()) / 2);
    }
    else
    {
      haOpen = Price::fromRaw((state.prevHaOpen.raw() + state.prevHaClose.raw()) / 2);
    }

    // HA_High = max(rawHigh, HA_Open, HA_Close)
    const auto haHigh = std::max({state.rawHigh, haOpen, haClose});

    // HA_Low = min(rawLow, HA_Open, HA_Close)
    const auto haLow = std::min({state.rawLow, haOpen, haClose});

    bar.open = haOpen;
    bar.high = haHigh;
    bar.low = haLow;
    bar.close = haClose;
  }

  TimePoint alignToInterval(TimePoint tp) const noexcept
  {
    const auto epoch = tp.time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch);
    const auto snapped = (ns.count() / _interval.count()) * _interval.count();
    return TimePoint(std::chrono::nanoseconds(snapped));
  }

  std::chrono::nanoseconds _interval;
  mutable SymbolStateMap<SymbolHAState> _symbolState;
};

static_assert(BarPolicy<HeikinAshiBarPolicy>, "HeikinAshiBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
