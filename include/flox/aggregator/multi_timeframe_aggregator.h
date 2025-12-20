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
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/aggregator/timeframe.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/strategy/symbol_state_map.h"

#include <array>
#include <chrono>
#include <variant>

namespace flox
{

template <size_t MaxTimeframes = 8>
class MultiTimeframeAggregator : public ISubsystem, public IMarketDataSubscriber
{
 public:
  static constexpr size_t kMaxTimeframes = MaxTimeframes;

  explicit MultiTimeframeAggregator(BarBus* bus) : _bus(bus) {}

  size_t addTimeInterval(std::chrono::seconds interval)
  {
    if (_numSlots >= MaxTimeframes)
    {
      return MaxTimeframes;
    }

    const size_t idx = _numSlots++;
    _slots[idx].policy = TimeBarPolicy(interval);
    _slots[idx].timeframeId = TimeframeId::time(interval);
    return idx;
  }

  size_t addTickInterval(uint32_t tickCount)
  {
    if (_numSlots >= MaxTimeframes)
    {
      return MaxTimeframes;
    }

    const size_t idx = _numSlots++;
    _slots[idx].policy = TickBarPolicy(tickCount);
    _slots[idx].timeframeId = TimeframeId::tick(tickCount);
    return idx;
  }

  size_t addVolumeInterval(double volumeThreshold)
  {
    if (_numSlots >= MaxTimeframes)
    {
      return MaxTimeframes;
    }

    const size_t idx = _numSlots++;
    _slots[idx].policy = VolumeBarPolicy::fromDouble(volumeThreshold);
    _slots[idx].timeframeId = TimeframeId::volume(static_cast<uint32_t>(volumeThreshold));
    return idx;
  }

  void start() override
  {
    for (size_t i = 0; i < _numSlots; ++i)
    {
      _slots[i].state.clear();
    }
  }

  void stop() override
  {
    for (size_t slotIdx = 0; slotIdx < _numSlots; ++slotIdx)
    {
      auto& slot = _slots[slotIdx];
      slot.state.forEach(
          [this, &slot, slotIdx](SymbolId symbol, SymbolState& state)
          {
            if (state.initialized)
            {
              emitBar(slotIdx, symbol, state);
              state.initialized = false;
            }
          });
      slot.state.clear();
    }
  }

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void onTrade(const TradeEvent& trade) override
  {
    for (size_t i = 0; i < _numSlots; ++i)
    {
      processSlot(i, trade);
    }
  }

  [[nodiscard]] std::array<TimeframeId, MaxTimeframes> timeframes() const noexcept
  {
    std::array<TimeframeId, MaxTimeframes> result{};
    for (size_t i = 0; i < _numSlots; ++i)
    {
      result[i] = _slots[i].timeframeId;
    }
    return result;
  }

  [[nodiscard]] size_t numTimeframes() const noexcept { return _numSlots; }

 private:
  using PolicyVariant = std::variant<TimeBarPolicy, TickBarPolicy, VolumeBarPolicy>;

  struct SymbolState
  {
    Bar bar{};
    InstrumentType instrument = InstrumentType::Spot;
    bool initialized = false;
  };

  struct Slot
  {
    PolicyVariant policy{TimeBarPolicy(std::chrono::seconds(60))};
    TimeframeId timeframeId{};
    SymbolStateMap<SymbolState> state;
  };

  void processSlot(size_t slotIdx, const TradeEvent& trade)
  {
    auto& slot = _slots[slotIdx];
    auto& state = slot.state[trade.trade.symbol];

    std::visit(
        [&](auto& policy)
        {
          if (!state.initialized) [[unlikely]]
          {
            policy.initBar(trade, state.bar);
            state.instrument = trade.trade.instrument;
            state.initialized = true;
            return;
          }

          if (policy.shouldClose(trade, state.bar)) [[unlikely]]
          {
            emitBar(slotIdx, trade.trade.symbol, state);
            policy.initBar(trade, state.bar);
            state.instrument = trade.trade.instrument;
            return;
          }

          policy.update(trade, state.bar);
        },
        slot.policy);
  }

  void emitBar(size_t slotIdx, SymbolId symbol, SymbolState& state)
  {
    state.bar.reason = BarCloseReason::Threshold;

    const auto& slot = _slots[slotIdx];
    BarEvent ev{.symbol = symbol,
                .instrument = state.instrument,
                .barType = slot.timeframeId.type,
                .barTypeParam = slot.timeframeId.param,
                .bar = state.bar};

    if (_bus) [[likely]]
    {
      _bus->publish(ev);
    }
  }

  std::array<Slot, MaxTimeframes> _slots{};
  size_t _numSlots = 0;
  BarBus* _bus;
};

}  // namespace flox
