/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"

namespace flox
{

struct BarEvent
{
  using Listener = IMarketDataSubscriber;

  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  BarType barType{};
  uint64_t barTypeParam{};  // interval in nanoseconds, tick count, volume threshold, etc.
  Bar bar{};

  uint64_t tickSequence = 0;  // internal, set by bus
};

// Lives here rather than in event_dispatcher.h: an EventBus names the
// dispatcher as a dependent type, so the specialization only has to be
// visible where a bus for THIS event is instantiated, and whoever has that
// bus already has this header.
template <typename T>
struct EventDispatcher;

template <>
struct EventDispatcher<BarEvent>
{
  // Templated on the subscriber so a statically-subscribed concrete type
  // keeps its identity all the way to the handler call (see subscribeStatic).
  template <typename Sub>
  static void dispatch(const BarEvent& ev, Sub& sub)
  {
    sub.onBar(ev);
  }
};

}  // namespace flox
