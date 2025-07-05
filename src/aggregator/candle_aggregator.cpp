#include "flox/aggregator/candle_aggregator.h"

#include <cassert>

namespace flox
{

CandleAggregator::CandleAggregator(std::chrono::seconds interval, CandleBusRef bus)
    : _interval(interval), _bus(bus)
{
  assert(_interval.count() > 0 && "CandleAggregator interval expected to be > 0");
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
      CandleEvent ev{id, partial->candle};
      _bus.publish(ev);
      partial.reset();
    }
  }

  std::vector<std::optional<PartialCandle>>{}.swap(_candles);
}

SubscriberId CandleAggregator::id() const
{
  return reinterpret_cast<SubscriberId>(this);
}

SubscriberMode CandleAggregator::mode() const
{
  return SubscriberMode::PUSH;
}

void CandleAggregator::onTrade(const TradeEvent& event)
{
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

  auto ts = alignToInterval(event.trade.timestamp);

  if (!partial->initialized || partial->candle.startTime != ts)
  {
    if (partial->initialized)
    {
      partial->candle.endTime = partial->candle.startTime + _interval;

      CandleEvent ev{event.trade.symbol, partial->candle};
      _bus.publish(ev);
    }

    partial->candle =
        Candle(ts, event.trade.price,
               Volume::fromDouble(event.trade.price.toDouble() * event.trade.quantity.toDouble()));
    partial->candle.endTime = ts + _interval;
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

std::chrono::steady_clock::time_point CandleAggregator::alignToInterval(
    std::chrono::steady_clock::time_point tp)
{
  auto epoch = tp.time_since_epoch();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch);

  assert(_interval.count() > 0 && "alignToInterval: interval must be > 0");

  auto snapped = (secs.count() / _interval.count()) * _interval.count();
  return std::chrono::steady_clock::time_point(std::chrono::seconds(snapped));
}

CandleAggregator::~CandleAggregator()
{
  stop();
}
}  // namespace flox
