/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/abstract_subscriber.h"

namespace flox
{

struct BookUpdateEvent;
struct TradeEvent;
struct CandleEvent;

class IMarketDataSubscriber : public ISubscriber
{
 public:
  virtual ~IMarketDataSubscriber() = default;

  virtual void onBookUpdate(const BookUpdateEvent& ev) {}
  virtual void onTrade(const TradeEvent& ev) {}
  virtual void onCandle(const CandleEvent& ev) {}
};

}  // namespace flox