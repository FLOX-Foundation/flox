/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"
#include "flox/engine/abstract_subscriber.h"

#include <string_view>

namespace flox
{

struct BookUpdateEvent;
struct TradeEvent;
struct BarEvent;

enum class MarketDataErrorCode
{
  UNKNOWN = 0,
  CONNECTION_LOST,
  CONNECTION_TIMEOUT,
  INVALID_MESSAGE,
  RATE_LIMITED,
  SUBSCRIPTION_FAILED,
  STALE_DATA
};

struct MarketDataError
{
  MarketDataErrorCode code{MarketDataErrorCode::UNKNOWN};
  SymbolId symbol{0};
  std::string message;
  int64_t timestampNs{0};
};

class IMarketDataSubscriber : public ISubscriber
{
 public:
  virtual ~IMarketDataSubscriber() = default;

  virtual void onBookUpdate(const BookUpdateEvent& ev) {}
  virtual void onTrade(const TradeEvent& ev) {}
  virtual void onBar(const BarEvent& ev) {}
  virtual void onMarketDataError(const MarketDataError& error) {}
};

}  // namespace flox