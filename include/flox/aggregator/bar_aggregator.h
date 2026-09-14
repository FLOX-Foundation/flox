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
#include "flox/aggregator/bar.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/policies/bps_range_bar_policy.h"
#include "flox/aggregator/policies/heikin_ashi_bar_policy.h"
#include "flox/aggregator/policies/range_bar_policy.h"
#include "flox/aggregator/policies/renko_bar_policy.h"
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/strategy/symbol_state_map.h"

#include <optional>
#include <vector>

namespace flox
{

template <typename Policy>
  requires BarPolicy<Policy>
class BarAggregator : public ISubsystem, public IMarketDataSubscriber
{
 public:
  BarAggregator(Policy policy, BarBus* bus) : _policy(std::move(policy)), _bus(bus) {}

  void start() override { _state.clear(); }

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

    if (_policy.shouldClose(trade, state.bar)) [[unlikely]]
    {
      emitBar(trade.trade.symbol, state);
      // Renko is the one policy whose close can gap past more than one
      // brick on a single trade; it alone offers gapBricks() to fill in the
      // ones a continuous price path would have produced. The other six
      // policies never gain this branch (`requires` fails to compile it for
      // them at all), so this cannot turn into an unbounded loop for a
      // policy whose very first trade already clears its threshold.
      if constexpr (requires(const Policy& p, const TradeEvent& t, const Bar& b) {
                      { p.gapBricks(t, b) } -> std::same_as<std::vector<Bar>>;
                    })
      {
        for (const Bar& synthetic : _policy.gapBricks(trade, state.bar))
        {
          publishBar(trade.trade.symbol, state.instrument, synthetic);
        }
      }
      _policy.initBar(trade, state.bar);
      state.instrument = trade.trade.instrument;
      return;
    }

    _policy.update(trade, state.bar);
  }

  const Policy& policy() const noexcept { return _policy; }

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
};

// Convenient type aliases
using TimeBarAggregator = BarAggregator<TimeBarPolicy>;
using TickBarAggregator = BarAggregator<TickBarPolicy>;
using VolumeBarAggregator = BarAggregator<VolumeBarPolicy>;
using RenkoBarAggregator = BarAggregator<RenkoBarPolicy>;
using RangeBarAggregator = BarAggregator<RangeBarPolicy>;
using BpsRangeBarAggregator = BarAggregator<BpsRangeBarPolicy>;
using HeikinAshiBarAggregator = BarAggregator<HeikinAshiBarPolicy>;

}  // namespace flox
