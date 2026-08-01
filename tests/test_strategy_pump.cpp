/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <vector>

#include "flox/backtest/strategy_pump.h"
#include "flox/engine/abstract_market_data_subscriber.h"

namespace
{

using namespace flox;

// A stateful signal whose result depends on delivery order: any dropped,
// duplicated, or reordered event changes the checksum.
struct EmaSignal
{
  int64_t emaFast = 0;
  int64_t emaSlow = 0;
  int64_t flips = 0;
  int64_t lastSignal = 0;
  uint64_t checksum = 0;

  void onTrade(const TradeEvent& ev)
  {
    const int64_t p = ev.trade.price.raw();
    emaFast += (p - emaFast) >> 3;
    emaSlow += (p - emaSlow) >> 6;
    const int64_t signal = emaFast > emaSlow ? 1 : -1;
    if (signal != lastSignal)
    {
      ++flips;
      lastSignal = signal;
    }
    checksum = checksum * 1315423911u + static_cast<uint64_t>(p);
  }

  void onBar(const BarEvent& ev)
  {
    const int64_t c = ev.bar.close.raw();
    emaFast += (c - emaFast) >> 3;
    checksum = checksum * 2654435761u + static_cast<uint64_t>(c);
  }
};

// The same signal delivered the way the dynamic engine delivers it.
struct VirtualEmaSignal final : IMarketDataSubscriber
{
  EmaSignal impl;
  void onTrade(const TradeEvent& ev) override { impl.onTrade(ev); }
  void onBar(const BarEvent& ev) override { impl.onBar(ev); }
  SubscriberId id() const override { return 1; }
};

std::vector<TradeEvent> makeTradeTape(size_t count)
{
  std::vector<TradeEvent> tape(count);
  std::mt19937_64 rng(0xF00Dull);
  int64_t price = 10'000'000;
  std::uniform_int_distribution<int64_t> step(-2000, 2000);
  for (auto& ev : tape)
  {
    price += step(rng);
    ev.trade.symbol = 7;
    ev.trade.price = Price::fromRaw(price);
    ev.trade.quantity = Quantity::fromRaw(1'000'000);
  }
  return tape;
}

std::vector<BarEvent> makeBarTape(size_t count)
{
  std::vector<BarEvent> tape(count);
  std::mt19937_64 rng(0xBA7Full);
  int64_t close = 10'000'000;
  std::uniform_int_distribution<int64_t> step(-5000, 5000);
  for (auto& ev : tape)
  {
    close += step(rng);
    ev.symbol = 7;
    ev.bar.close = Price::fromRaw(close);
  }
  return tape;
}

class FakeReader : public replay::IMultiSegmentReader
{
 public:
  explicit FakeReader(std::vector<replay::ReplayEvent> events)
      : _events(std::move(events)) {}

  uint64_t forEach(EventCallback callback) override
  {
    uint64_t n = 0;
    for (const auto& ev : _events)
    {
      ++n;
      if (!callback(ev))
      {
        break;
      }
    }
    return n;
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    uint64_t n = 0;
    for (const auto& ev : _events)
    {
      if (ev.timestamp_ns < start_ts_ns)
      {
        continue;
      }
      ++n;
      if (!callback(ev))
      {
        break;
      }
    }
    return n;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segments; }
  uint64_t totalEvents() const override { return _events.size(); }

 private:
  std::vector<replay::ReplayEvent> _events;
  std::vector<replay::SegmentInfo> _segments;
};

TEST(StrategyPump, TradeTapeMatchesVirtualDelivery)
{
  const auto tape = makeTradeTape(50'000);

  VirtualEmaSignal reference;
  IMarketDataSubscriber* erased = &reference;
  for (const auto& ev : tape)
  {
    erased->onTrade(ev);
  }

  EmaSignal mono;
  StrategyPump<EmaSignal> pump(mono);
  const auto stats = pump.runTrades(tape);

  EXPECT_EQ(stats.trades, tape.size());
  EXPECT_EQ(stats.total(), tape.size());
  EXPECT_EQ(mono.checksum, reference.impl.checksum);
  EXPECT_EQ(mono.flips, reference.impl.flips);
  EXPECT_EQ(mono.emaFast, reference.impl.emaFast);
  EXPECT_EQ(mono.emaSlow, reference.impl.emaSlow);
}

TEST(StrategyPump, BarTapeMatchesVirtualDelivery)
{
  const auto tape = makeBarTape(10'000);

  VirtualEmaSignal reference;
  for (const auto& ev : tape)
  {
    static_cast<IMarketDataSubscriber&>(reference).onBar(ev);
  }

  EmaSignal mono;
  StrategyPump<EmaSignal> pump(mono);
  const auto stats = pump.runBars(tape);

  EXPECT_EQ(stats.bars, tape.size());
  EXPECT_EQ(mono.checksum, reference.impl.checksum);
}

TEST(StrategyPump, ReaderPathConvertsTradesAndSkipsOtherRecords)
{
  std::vector<replay::ReplayEvent> events;
  std::mt19937_64 rng(0x5EEDull);
  int64_t price = 20'000'000;
  std::uniform_int_distribution<int64_t> step(-1000, 1000);
  for (int i = 0; i < 1'000; ++i)
  {
    price += step(rng);
    replay::ReplayEvent ev;
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = i;
    ev.trade.symbol_id = 7;
    ev.trade.price_raw = price;
    ev.trade.qty_raw = 1'000'000;
    ev.trade.side = static_cast<uint8_t>(i % 2);
    events.push_back(ev);

    if (i % 10 == 0)
    {
      replay::ReplayEvent book;
      book.type = replay::EventType::BookDelta;
      book.timestamp_ns = i;
      events.push_back(book);
    }
  }
  FakeReader reader(std::move(events));

  // Reference: the same conversion applied by hand, delivered virtually.
  VirtualEmaSignal reference;
  FakeReader referenceReader(
      [&]
      {
        std::vector<replay::ReplayEvent> copy;
        reader.forEach(
            [&](const replay::ReplayEvent& ev)
            {
              copy.push_back(ev);
              return true;
            });
        return copy;
      }());
  referenceReader.forEach(
      [&](const replay::ReplayEvent& ev)
      {
        if (ev.type == replay::EventType::Trade)
        {
          TradeEvent t;
          t.trade.symbol = ev.trade.symbol_id;
          t.trade.price = Price::fromRaw(ev.trade.price_raw);
          t.trade.quantity = Quantity::fromRaw(ev.trade.qty_raw);
          t.trade.isBuy = (ev.trade.side == 1);
          static_cast<IMarketDataSubscriber&>(reference).onTrade(t);
        }
        return true;
      });

  EmaSignal mono;
  StrategyPump<EmaSignal> pump(mono);
  const auto stats = pump.run(reader);

  EXPECT_EQ(stats.trades, 1'000u);
  EXPECT_EQ(stats.skipped, 100u);
  EXPECT_EQ(stats.total(), 1'100u);
  EXPECT_EQ(mono.checksum, reference.impl.checksum);
}

}  // namespace
