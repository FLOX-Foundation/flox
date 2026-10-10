/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/aggregator/aggregation_policy.h"
#include "lrvx/aggregator/bar.h"
#include "lrvx/aggregator/bus/bar_bus.h"
#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/aggregator/policies/bps_range_bar_policy.h"
#include "lrvx/aggregator/policies/heikin_ashi_bar_policy.h"
#include "lrvx/aggregator/policies/range_bar_policy.h"
#include "lrvx/aggregator/policies/renko_bar_policy.h"
#include "lrvx/aggregator/policies/tick_bar_policy.h"
#include "lrvx/aggregator/policies/time_bar_policy.h"
#include "lrvx/aggregator/policies/volume_bar_policy.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/common.h"
#include "lrvx/engine/abstract_market_data_subscriber.h"
#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/strategy/symbol_state_map.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace lrvx
{

template <typename Policy>
  requires BarPolicy<Policy>
class BarAggregator : public ISubsystem, public IMarketDataSubscriber
{
 public:
  BarAggregator(Policy policy, BarBus* bus) : _policy(std::move(policy)), _bus(bus) {}

  void start() override
  {
    _state.clear();
    _lateTradeCount = 0;
  }

  void stop() override
  {
    // Flush all partial bars
    _state.forEach(
        [this](SymbolId symbol, SymbolState& state)
        {
          if (state.initialized)
          {
            emitBar(symbol, state, BarCloseReason::Forced);
            state.initialized = false;
          }
        });
    _state.clear();
  }

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void onTrade(const TradeEvent& trade) override
  {
    auto& state = _state[trade.trade.symbol];

    if (!state.initialized) [[unlikely]]
    {
      _policy.initBar(trade, state.bar);
      state.instrument = trade.trade.instrument;
      state.initialized = true;
      return;
    }

    if constexpr (DetectsLateTrades<Policy>)
    {
      // Dropped, not folded in: see TimeBarPolicy::isLate.
      if (_policy.isLate(trade, state.bar)) [[unlikely]]
      {
        ++_lateTradeCount;
        return;
      }
    }

    if (_policy.shouldClose(trade, state.bar)) [[unlikely]]
    {
      if constexpr (ClosesAndReopens<Policy>)
      {
        // Renko is the one policy whose close is not "publish the bar as it
        // stands": the brick has to be finished at the boundary and the next
        // one opened there, and a single trade can span several bricks. The
        // policy does all of it and hands back every bar to publish, so this
        // copy of the close path cannot drift from the other three.
        const InstrumentType instrument = state.instrument;
        const SymbolId symbol = trade.trade.symbol;
        _policy.closeAndReopen(trade, state.bar,
                               [&](const Bar& bar)
                               { publishBar(symbol, instrument, bar); });
      }
      else
      {
        emitBar(trade.trade.symbol, state);
        _policy.initBar(trade, state.bar);
      }
      state.instrument = trade.trade.instrument;
      return;
    }

    _policy.update(trade, state.bar);
  }

  const Policy& policy() const noexcept { return _policy; }

  // Trades dropped because they belonged to a bar that was already gone.
  // Always 0 for a policy with no notion of a late trade. Losing feed data
  // silently is exactly what a cross-venue merge produces, so the count is
  // readable rather than implicit.
  std::uint64_t lateTradeCount() const noexcept { return _lateTradeCount; }

 private:
  struct SymbolState
  {
    Bar bar{};
    InstrumentType instrument = InstrumentType::Spot;
    bool initialized = false;
  };

  void emitBar(SymbolId symbol, SymbolState& state, BarCloseReason reason = BarCloseReason::Threshold)
  {
    state.bar.reason = reason;
    publishBar(symbol, state.instrument, state.bar);
  }

  void publishBar(SymbolId symbol, InstrumentType instrument, const Bar& bar)
  {
    BarEvent ev{.symbol = symbol,
                .instrument = instrument,
                .barType = Policy::kBarType,
                .barTypeParam = _policy.param(),
                .bar = bar};

    if (_bus) [[likely]]
    {
      _bus->publish(ev);
    }
  }

  Policy _policy;
  BarBus* _bus;
  SymbolStateMap<SymbolState> _state;
  std::uint64_t _lateTradeCount = 0;
};

// Convenient type aliases
using TimeBarAggregator = BarAggregator<TimeBarPolicy>;
using TickBarAggregator = BarAggregator<TickBarPolicy>;
using VolumeBarAggregator = BarAggregator<VolumeBarPolicy>;
using RenkoBarAggregator = BarAggregator<RenkoBarPolicy>;
using RangeBarAggregator = BarAggregator<RangeBarPolicy>;
using BpsRangeBarAggregator = BarAggregator<BpsRangeBarPolicy>;
using HeikinAshiBarAggregator = BarAggregator<HeikinAshiBarPolicy>;

}  // namespace lrvx
