/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <span>

#include "flox/aggregator/events/bar_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/replay/abstract_event_reader.h"

namespace flox
{

/// A strategy needs at least one of these handlers. Which ones exist is
/// checked at compile time.
template <typename S>
concept TradePumpable = requires(S s, const TradeEvent& ev) {
  s.onTrade(ev);
};

template <typename S>
concept BarPumpable = requires(S s, const BarEvent& ev) {
  s.onBar(ev);
};

template <typename S>
concept Pumpable = TradePumpable<S> || BarPumpable<S>;

struct StrategyPumpStats
{
  uint64_t trades{0};
  uint64_t bars{0};
  uint64_t skipped{0};

  uint64_t total() const { return trades + bars + skipped; }
};

/// Replays a tape into a concrete strategy type. The strategy is a template
/// parameter, so the compiler inlines the handler into the replay loop and
/// keeps its state in registers between events. On an EMA-cross handler that
/// is 2-3.7x over delivering the same events through a virtual interface,
/// depending on the machine (benchmarks/batch_delivery_benchmark.cpp has the
/// comparison). Use it for indicator studies and parameter sweeps, where the
/// strategy is the only per-event work.
///
/// There is no order simulation, no fills, no positions, no clocks. A
/// strategy that trades needs BacktestRunner. The pump only tells you what a
/// signal does over a tape.
///
/// Strategy is any type with onTrade(const TradeEvent&) and/or
/// onBar(const BarEvent&). No inheritance needed. The pump keeps a raw
/// pointer, so the strategy has to outlive it.
template <Pumpable Strategy>
class StrategyPump
{
 public:
  explicit StrategyPump(Strategy& strategy) : _strategy(&strategy) {}

  /// Deliver every trade event in the span, in order.
  StrategyPumpStats runTrades(std::span<const TradeEvent> tape)
    requires TradePumpable<Strategy>
  {
    StrategyPumpStats stats;
    // restrict lets the optimizer keep the strategy's state in registers
    // across the loop: without it, every event pays loads and stores because
    // the strategy could alias the tape.
    Strategy* __restrict s = _strategy;
    for (const TradeEvent& ev : tape)
    {
      s->onTrade(ev);
    }
    stats.trades = tape.size();
    return stats;
  }

  /// Deliver every bar event in the span, in order.
  StrategyPumpStats runBars(std::span<const BarEvent> tape)
    requires BarPumpable<Strategy>
  {
    StrategyPumpStats stats;
    Strategy* __restrict s = _strategy;
    for (const BarEvent& ev : tape)
    {
      s->onBar(ev);
    }
    stats.bars = tape.size();
    return stats;
  }

  /// Replay a recorded .floxlog tape into the strategy. Trade records
  /// convert the same way BacktestRunner::processEvent converts them. Every
  /// other record type (book, option quote, pool state) goes into
  /// stats.skipped: replaying books needs the pooled-event machinery, and
  /// the pump does not carry it.
  StrategyPumpStats run(replay::IMultiSegmentReader& reader)
    requires TradePumpable<Strategy>
  {
    StrategyPumpStats stats;
    reader.forEach(
        [&](const replay::ReplayEvent& event)
        {
          if (event.type == replay::EventType::Trade)
          {
            TradeEvent ev;
            ev.trade.symbol = event.trade.symbol_id;
            ev.trade.price = Price::fromRaw(event.trade.price_raw);
            ev.trade.quantity = Quantity::fromRaw(event.trade.qty_raw);
            ev.trade.isBuy = (event.trade.side == 1);
            ev.trade.exchangeTsNs = event.trade.exchange_ts_ns;
            ev.trade.instrument = static_cast<InstrumentType>(event.trade.instrument);
            ev.exchangeMsgTsNs = event.trade.exchange_ts_ns;
            _strategy->onTrade(ev);
            ++stats.trades;
          }
          else
          {
            ++stats.skipped;
          }
          return true;
        });
    return stats;
  }

 private:
  Strategy* _strategy;
};

}  // namespace flox
