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
#include "flox/util/performance/force_inline.h"

#include <array>
#include <chrono>
#include <memory>

namespace flox
{

template <size_t MaxTimeframes = 8>
class MultiTimeframeAggregator : public ISubsystem, public IMarketDataSubscriber
{
 public:
  static constexpr size_t kMaxTimeframes = MaxTimeframes;

  explicit MultiTimeframeAggregator(BarBus* bus)
      : _slots(std::make_unique<SlotsArray>()), _bus(bus)
  {
  }

 private:
  enum class PolicyTag : uint8_t
  {
    Time,
    Tick,
    Volume
  };

  union PolicyStorage
  {
    TimeBarPolicy time;
    TickBarPolicy tick;
    VolumeBarPolicy volume;

    PolicyStorage() : time(std::chrono::seconds(60)) {}
    ~PolicyStorage() {}
  };

  struct SymbolState
  {
    Bar bar{};
    InstrumentType instrument = InstrumentType::Spot;
    bool initialized = false;
  };

  struct Slot
  {
    PolicyStorage storage{};
    PolicyTag tag = PolicyTag::Time;
    TimeframeId timeframeId{};
    SymbolStateMap<SymbolState> state{};
  };

  using SlotsArray = std::array<Slot, MaxTimeframes>;

  FLOX_FORCE_INLINE SlotsArray& slots() noexcept { return *_slots; }
  FLOX_FORCE_INLINE const SlotsArray& slots() const noexcept { return *_slots; }

 public:
  size_t addTimeInterval(std::chrono::seconds interval)
  {
    if (_numSlots >= MaxTimeframes)
    {
      return MaxTimeframes;
    }

    const size_t idx = _numSlots++;
    auto& slot = slots()[idx];
    slot.tag = PolicyTag::Time;
    new (&slot.storage.time) TimeBarPolicy(interval);
    slot.timeframeId = TimeframeId::time(interval);
    return idx;
  }

  size_t addTickInterval(uint32_t tickCount)
  {
    if (_numSlots >= MaxTimeframes)
    {
      return MaxTimeframes;
    }

    const size_t idx = _numSlots++;
    auto& slot = slots()[idx];
    slot.tag = PolicyTag::Tick;
    new (&slot.storage.tick) TickBarPolicy(tickCount);
    slot.timeframeId = TimeframeId::tick(tickCount);
    return idx;
  }

  size_t addVolumeInterval(double volumeThreshold)
  {
    if (_numSlots >= MaxTimeframes)
    {
      return MaxTimeframes;
    }

    const size_t idx = _numSlots++;
    auto& slot = slots()[idx];
    slot.tag = PolicyTag::Volume;
    new (&slot.storage.volume) VolumeBarPolicy(VolumeBarPolicy::fromDouble(volumeThreshold));
    slot.timeframeId = TimeframeId::volume(static_cast<uint32_t>(volumeThreshold));
    return idx;
  }

  void start() override
  {
    for (size_t i = 0; i < _numSlots; ++i)
    {
      slots()[i].state.clear();
    }
  }

  void stop() override
  {
    for (size_t slotIdx = 0; slotIdx < _numSlots; ++slotIdx)
    {
      auto& slot = slots()[slotIdx];
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
      result[i] = slots()[i].timeframeId;
    }
    return result;
  }

  [[nodiscard]] size_t numTimeframes() const noexcept { return _numSlots; }

 private:
  FLOX_FORCE_INLINE void processSlot(size_t slotIdx, const TradeEvent& trade)
  {
    auto& slot = slots()[slotIdx];
    auto& state = slot.state[trade.trade.symbol];

    switch (slot.tag)
    {
      case PolicyTag::Time:
        processPolicy(slot.storage.time, slot, slotIdx, trade, state);
        break;
      case PolicyTag::Tick:
        processPolicy(slot.storage.tick, slot, slotIdx, trade, state);
        break;
      case PolicyTag::Volume:
        processPolicy(slot.storage.volume, slot, slotIdx, trade, state);
        break;
    }
  }

  template <typename Policy>
  FLOX_FORCE_INLINE void processPolicy(Policy& policy, Slot& slot, size_t slotIdx,
                                       const TradeEvent& trade, SymbolState& state)
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
  }

  void emitBar(size_t slotIdx, SymbolId symbol, SymbolState& state)
  {
    state.bar.reason = BarCloseReason::Threshold;

    const auto& slot = slots()[slotIdx];
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

  std::unique_ptr<SlotsArray> _slots;
  size_t _numSlots = 0;
  BarBus* _bus;
};

}  // namespace flox
