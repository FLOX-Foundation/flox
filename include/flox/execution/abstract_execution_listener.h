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
#include "flox/execution/order.h"

namespace flox
{

struct OrderEvent;  // fwd decl — full definition lives in events/order_event.h

class IOrderExecutionListener : public ISubscriber
{
  SubscriberId _id{};

 public:
  IOrderExecutionListener(SubscriberId id) : _id(id) {}

  virtual ~IOrderExecutionListener() = default;

  SubscriberId id() const override { return _id; };

  virtual void onOrderSubmitted(const Order&) {}
  virtual void onOrderAccepted(const Order&) {}
  virtual void onOrderPartiallyFilled(const Order&, Quantity) {}
  virtual void onOrderFilled(const Order&) {}
  virtual void onOrderPendingCancel(const Order&) {}
  virtual void onOrderCanceled(const Order&) {}
  virtual void onOrderExpired(const Order&) {}
  virtual void onOrderRejected(const Order&, const std::string&) {}
  virtual void onOrderReplaced(const Order&, const Order&) {}

  // Conditional order callbacks
  virtual void onOrderPendingTrigger(const Order&) {}
  virtual void onOrderTriggered(const Order&) {}
  virtual void onTrailingStopUpdated(const Order&, Price /*newTriggerPrice*/) {}

  // Resting-order queue position moved without any other lifecycle
  // transition. `queueAhead` is the volume currently in front of the
  // order at its price level; `queueTotal` is the level's total
  // quantity. Both are in raw fixed-point units. Backtest only — live
  // exchanges do not generally publish queue position.
  virtual void onOrderQueuePositionChange(const Order&, Quantity /*queueAhead*/,
                                          Quantity /*queueTotal*/) {}

  // Resting-order categorical market position transitioned (best /
  // behind_best / mid_spread / level_empty / crossed). The position
  // value lives in events/order_event.h (MarketPosition enum); a
  // forward int8 is used here to keep this header free of that
  // include. distanceToBestTicks is signed ticks from best on our
  // side. Backtest only.
  virtual void onOrderMarketPositionChange(const Order&, uint8_t /*position*/,
                                           int32_t /*distanceToBestTicks*/) {}

  // Replace-in-flight lifecycle. SUBMITTED fires immediately on
  // replaceOrder(); ACCEPTED fires after the ack latency on
  // successful replacement; REJECTED fires when the replace cannot
  // complete (typically because the original order filled in the
  // ack window). The terminal REPLACED status still fires through
  // onOrderReplaced(). Backtest only — live exchanges have their
  // own replace semantics.
  virtual void onOrderReplaceSubmitted(const Order& /*oldOrder*/,
                                       const Order& /*newOrder*/) {}
  virtual void onOrderReplaceAccepted(const Order& /*oldOrder*/,
                                      const Order& /*newOrder*/) {}
  virtual void onOrderReplaceRejected(const Order& /*oldOrder*/,
                                      const Order& /*newOrder*/,
                                      const std::string& /*reason*/) {}

  // Raw OrderEvent fan-out for listeners that need access to all
  // payload fields at once (queue position + timestamps + maker/taker
  // + reject reason). Default empty so existing listeners that only
  // override the typed callbacks above keep working unchanged.
  // Fires AFTER the typed dispatch — see BacktestRunner.
  virtual void onOrderEvent(const OrderEvent& /*ev*/) {}
};

}  // namespace flox