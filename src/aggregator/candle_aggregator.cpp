/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/aggregator/candle_aggregator.h"
#include "flox/common.h"
#include "flox/util/performance/profile.h"

#include <cassert>

namespace flox
{

CandleAggregator::CandleAggregator(std::chrono::seconds interval, CandleBus* bus)
    : _interval(interval), _bus(bus)
{
}

void CandleAggregator::start()
{
  std::vector<std::optional<PartialCandle>>{}.swap(_candles);
}

void CandleAggregator::stop()
{
  for (SymbolId id = 0; id < _candles.size(); ++id)
  {
    auto& partial = _candles[id];
    if (partial && partial->initialized)
    {
      partial->candle.endTime = partial->candle.startTime + _interval;
      CandleEvent ev{
          .symbol = id,
          .instrument = partial->instrument,
          .candle = partial->candle};
      _bus->publish(ev);
      partial.reset();
    }
  }

  std::vector<std::optional<PartialCandle>>{}.swap(_candles);
}

void CandleAggregator::onTrade(const TradeEvent& event)
{
  FLOX_PROFILE_SCOPE("CandleAggregator::onTrade");

  auto id = event.trade.symbol;
  if (id >= _candles.size())
  {
    _candles.resize(id + 1);
  }

  auto& partial = _candles[id];
  if (!partial)
  {
    partial.emplace();
  }

  auto ts = alignToInterval(fromUnixNs(event.trade.exchangeTsNs));

  if (!partial->initialized || partial->candle.startTime != ts)
  {
    if (partial->initialized)
    {
      partial->candle.endTime = partial->candle.startTime + _interval;

      CandleEvent ev{
          .symbol = event.trade.symbol,
          .instrument = event.trade.instrument,
          .candle = partial->candle};
      _bus->publish(ev);
    }

    partial->candle =
        Candle(ts, event.trade.price,
               Volume::fromDouble(event.trade.price.toDouble() * event.trade.quantity.toDouble()));
    partial->candle.endTime = ts + _interval;
    partial->instrument = event.trade.instrument;
    partial->initialized = true;

    return;
  }

  auto& c = partial->candle;
  c.high = std::max(c.high, event.trade.price);
  c.low = std::min(c.low, event.trade.price);
  c.close = event.trade.price;
  c.volume += Volume::fromDouble(event.trade.price.toDouble() * event.trade.quantity.toDouble());
  c.endTime = partial->candle.startTime + _interval;
}

TimePoint CandleAggregator::alignToInterval(TimePoint tp)
{
  auto epoch = tp.time_since_epoch();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch);
  auto snapped = (secs.count() / _interval.count()) * _interval.count();
  return TimePoint(std::chrono::seconds(snapped));
}

}  // namespace flox