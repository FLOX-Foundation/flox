/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace flox::testing
{

// Cross-binding parity test fixture for the documented bar-close
// dispatch order (registration order on tied closes). Wraps a BarBus,
// a MultiTimeframeAggregator, and a recorder so a binding only needs
// to register timeframes, push trades, and read back the dispatch
// sequence.
class BarDispatchRecorder
{
 public:
  struct Entry
  {
    uint8_t barType;        // matches flox::BarType
    uint64_t barTypeParam;  // seconds for time, count for tick, etc.
  };

  BarDispatchRecorder() : _bus(), _agg(&_bus), _rec(_entries) { _bus.enableDrainOnStop(); }

  size_t addTimeIntervalSeconds(uint32_t seconds)
  {
    return _agg.addTimeInterval(std::chrono::seconds(seconds));
  }

  void onTrade(uint32_t symbol, double price, double qty, int64_t tsNs)
  {
    if (!_started)
    {
      _bus.subscribe(&_rec);
      _bus.start();
      _agg.start();
      _started = true;
    }
    TradeEvent ev{};
    ev.trade.symbol = symbol;
    ev.trade.price = Price::fromDouble(price);
    ev.trade.quantity = Quantity::fromDouble(qty);
    ev.trade.exchangeTsNs = UnixNanos{tsNs};
    ev.trade.isBuy = true;
    ev.trade.instrument = InstrumentType::Spot;
    _agg.onTrade(ev);
  }

  void finalize()
  {
    if (!_finalized)
    {
      if (_started)
      {
        _agg.stop();
        _bus.stop();
      }
      _finalized = true;
    }
  }

  size_t count() const { return _entries.size(); }
  uint8_t typeAt(size_t i) const { return _entries[i].barType; }
  uint64_t paramAt(size_t i) const { return _entries[i].barTypeParam; }

 private:
  class Recorder : public IMarketDataSubscriber
  {
   public:
    explicit Recorder(std::vector<Entry>& sink) : _sink(sink) {}
    SubscriberId id() const override { return 1; }
    void onBar(const BarEvent& ev) override
    {
      _sink.push_back({static_cast<uint8_t>(ev.barType), ev.barTypeParam});
    }

   private:
    std::vector<Entry>& _sink;
  };

  BarBus _bus;
  MultiTimeframeAggregator<8> _agg;
  std::vector<Entry> _entries;
  Recorder _rec;
  bool _started = false;
  bool _finalized = false;
};

}  // namespace flox::testing
