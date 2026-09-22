/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/util/memory/pool.h"

namespace flox
{

// How an EventBus hands one event to one listener. The bus names it as a
// dependent type, so only the specialization for the event a given bus
// carries has to be visible where that bus is instantiated.
//
// The specializations therefore live next to their event -- BarEvent's in
// flox/aggregator/events/bar_event.h, OrderEvent's in
// flox/execution/events/order_event.h, and so on -- and not here. They used
// to live here, which made every EventBus in the tree include every event
// type in the tree: a venue shard carrying its own command and engine-event
// types pulled in bars, book updates and trades it never sees. Anyone who
// holds an EventBus<E> already has E's header, so nothing needs the old
// gather-everything include.
template <typename T>
struct EventDispatcher;

template <typename T>
struct EventDispatcher<pool::Handle<T>>
{
  template <typename Sub>
  static void dispatch(const pool::Handle<T>& ev, Sub& sub)
  {
    EventDispatcher<T>::dispatch(*ev, sub);
  }
};

}  // namespace flox
