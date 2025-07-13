/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bus/candle_bus.h"
#include "flox/book/candle.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/abstract_subsystem.h"

#include <chrono>
#include <optional>
#include <vector>

namespace flox
{

class CandleAggregator : public ISubsystem, public IMarketDataSubscriber
{
 public:
  CandleAggregator(std::chrono::seconds interval, CandleBus* bus);

  void start() override;
  void stop() override;

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }
  SubscriberMode mode() const override { return SubscriberMode::PUSH; }

  void onTrade(const TradeEvent& trade) override;

 private:
  struct PartialCandle
  {
    Candle candle;
    bool initialized = false;
  };

  std::chrono::seconds _interval;
  CandleBus* _bus = nullptr;
  std::vector<std::optional<PartialCandle>> _candles;

  std::chrono::steady_clock::time_point alignToInterval(std::chrono::steady_clock::time_point tp);
};

}  // namespace flox
