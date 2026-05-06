/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/ohlcv_replay_source.h"

#include "flox/common.h"

namespace flox
{

OhlcvReplaySource::OhlcvReplaySource(std::vector<Bar> bars)
    : _bars(std::move(bars))
{
}

uint64_t OhlcvReplaySource::forEach(EventCallback cb)
{
  uint64_t n = 0;
  for (const auto& b : _bars)
  {
    if (!cb(make(b)))
    {
      break;
    }
    ++n;
  }
  return n;
}

uint64_t OhlcvReplaySource::forEachFrom(int64_t start_ns, EventCallback cb)
{
  uint64_t n = 0;
  for (const auto& b : _bars)
  {
    if (b.ts_ns < start_ns)
    {
      continue;
    }
    if (!cb(make(b)))
    {
      break;
    }
    ++n;
  }
  return n;
}

replay::ReplayEvent OhlcvReplaySource::make(const Bar& b)
{
  replay::ReplayEvent ev{};
  ev.type = replay::EventType::Trade;
  ev.timestamp_ns = b.ts_ns;
  ev.trade.exchange_ts_ns = b.ts_ns;
  ev.trade.price_raw = b.price_raw;
  ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();
  ev.trade.symbol_id = b.symbol_id;
  ev.trade.side = 1;
  return ev;
}

}  // namespace flox
