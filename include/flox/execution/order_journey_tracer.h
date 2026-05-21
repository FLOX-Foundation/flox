/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/events/order_event.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace flox
{

// One row of the order-journey trace. Compact, fixed-size; mirrors
// the OrderEvent fields a researcher needs for post-trade forensics.
struct OrderTraceRecord
{
  OrderId orderId{};
  uint32_t seq{};     // 0-based position within this order's trace
  uint8_t status{};   // OrderEventStatus
  uint8_t isMaker{};  // 0 taker, 1 maker; meaningful on fill statuses
  uint8_t _pad[2]{};
  int64_t tsNs{};  // event emission time (exchangeTsNs)
  int64_t fillQtyRaw{};
  int64_t fillPriceRaw{};
  int64_t queueAheadRaw{};
  int64_t queueTotalRaw{};
  OrderTimestamps timestamps{};
};

// Listener that records the full event sequence of every order it
// observes into per-order ring buffers. Attach to a BacktestRunner
// via addExecutionListener(). Designed for post-trade forensics:
// median ack latency, maker fill ratio, cancel-race loss rate.
//
// Bounded memory: maxOrders limits how many distinct orders the
// tracer holds; oldest order (by first seen) is evicted when the
// cap is hit. Per-order trace length is bounded by maxRecordsPerOrder.
//
// Sampling: sampleRate in [0, 1] selects orders deterministically via
// `(orderId * salt) % 1000 / 1000`. Default 1.0 keeps every order.
class OrderJourneyTracer : public IOrderExecutionListener
{
 public:
  static constexpr SubscriberId kDefaultId = 0xFEED;

  struct Config
  {
    size_t maxOrders{1'000'000};
    size_t maxRecordsPerOrder{64};
    double sampleRate{1.0};
    uint64_t sampleSalt{0x9E3779B97F4A7C15ULL};
  };

  OrderJourneyTracer();
  explicit OrderJourneyTracer(Config cfg, SubscriberId id = kDefaultId);

  void onOrderEvent(const OrderEvent& ev) override;

  // Read the full trace for a single order. Returns empty vector if
  // unknown or sampled-out.
  std::vector<OrderTraceRecord> journey(OrderId id) const;

  // Flatten every recorded order into one contiguous trace, ordered
  // first by first-seen orderId then by per-order seq.
  std::vector<OrderTraceRecord> result() const;

  // Aggregate analytics over the recorded trace.
  size_t orderCount() const;
  size_t recordCount() const;
  double medianAckLatencyNs() const;
  double medianTimeToFirstFillNs() const;
  double makerFillRatio() const;
  double cancelRaceLossRate() const;

  void clear();

 private:
  bool shouldRecord(OrderId id) const;

  Config _cfg;
  // First-seen order ordering for LRU-style cap eviction.
  std::deque<OrderId> _insertionOrder;
  std::unordered_map<OrderId, std::vector<OrderTraceRecord>> _byOrder;
};

}  // namespace flox
