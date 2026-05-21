/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/order_journey_tracer.h"

#include <algorithm>
#include <cmath>

namespace flox
{

OrderJourneyTracer::OrderJourneyTracer() : OrderJourneyTracer(Config{}, kDefaultId)
{
}

OrderJourneyTracer::OrderJourneyTracer(Config cfg, SubscriberId id)
    : IOrderExecutionListener(id), _cfg(cfg)
{
  if (_cfg.sampleRate < 0.0)
  {
    _cfg.sampleRate = 0.0;
  }
  if (_cfg.sampleRate > 1.0)
  {
    _cfg.sampleRate = 1.0;
  }
  if (_cfg.maxOrders == 0)
  {
    _cfg.maxOrders = 1;
  }
  if (_cfg.maxRecordsPerOrder == 0)
  {
    _cfg.maxRecordsPerOrder = 1;
  }
}

bool OrderJourneyTracer::shouldRecord(OrderId id) const
{
  if (_cfg.sampleRate >= 1.0)
  {
    return true;
  }
  if (_cfg.sampleRate <= 0.0)
  {
    return false;
  }
  // Hash orderId with the salt and mod to a 1000-bucket grid; compare
  // against the threshold. Stable across runs given the same salt.
  const uint64_t bucket = (static_cast<uint64_t>(id) * _cfg.sampleSalt) % 1000ULL;
  return bucket < static_cast<uint64_t>(_cfg.sampleRate * 1000.0);
}

void OrderJourneyTracer::onOrderEvent(const OrderEvent& ev)
{
  const OrderId id = ev.order.id;
  if (!shouldRecord(id))
  {
    return;
  }

  auto it = _byOrder.find(id);
  if (it == _byOrder.end())
  {
    if (_byOrder.size() >= _cfg.maxOrders)
    {
      // Evict the oldest tracked order.
      while (!_insertionOrder.empty())
      {
        const OrderId oldest = _insertionOrder.front();
        _insertionOrder.pop_front();
        if (_byOrder.erase(oldest) > 0)
        {
          break;
        }
      }
    }
    _insertionOrder.push_back(id);
    it = _byOrder.emplace(id, std::vector<OrderTraceRecord>{}).first;
    it->second.reserve(8);
  }

  auto& trace = it->second;
  if (trace.size() >= _cfg.maxRecordsPerOrder)
  {
    return;
  }

  OrderTraceRecord rec{};
  rec.orderId = id;
  rec.seq = static_cast<uint32_t>(trace.size());
  rec.status = static_cast<uint8_t>(ev.status);
  rec.isMaker = ev.isMaker ? 1 : 0;
  rec.tsNs = ev.exchangeTsNs;
  rec.fillQtyRaw = ev.fillQty.raw();
  rec.fillPriceRaw = ev.fillPrice.raw();
  rec.queueAheadRaw = ev.queueAhead.raw();
  rec.queueTotalRaw = ev.queueTotal.raw();
  rec.timestamps = ev.timestamps;
  trace.push_back(rec);
}

std::vector<OrderTraceRecord> OrderJourneyTracer::journey(OrderId id) const
{
  auto it = _byOrder.find(id);
  if (it == _byOrder.end())
  {
    return {};
  }
  return it->second;
}

std::vector<OrderTraceRecord> OrderJourneyTracer::result() const
{
  std::vector<OrderTraceRecord> out;
  size_t total = 0;
  for (const auto& [_, v] : _byOrder)
  {
    total += v.size();
  }
  out.reserve(total);
  for (OrderId id : _insertionOrder)
  {
    auto it = _byOrder.find(id);
    if (it == _byOrder.end())
    {
      continue;
    }
    for (const auto& rec : it->second)
    {
      out.push_back(rec);
    }
  }
  return out;
}

size_t OrderJourneyTracer::orderCount() const
{
  return _byOrder.size();
}

size_t OrderJourneyTracer::recordCount() const
{
  size_t total = 0;
  for (const auto& [_, v] : _byOrder)
  {
    total += v.size();
  }
  return total;
}

namespace
{
double computeMedian(std::vector<int64_t>& xs)
{
  if (xs.empty())
  {
    return std::nan("");
  }
  const size_t mid = xs.size() / 2;
  std::nth_element(xs.begin(), xs.begin() + mid, xs.end());
  const double m1 = static_cast<double>(xs[mid]);
  if (xs.size() % 2 == 1)
  {
    return m1;
  }
  std::nth_element(xs.begin(), xs.begin() + mid - 1, xs.end());
  return 0.5 * (m1 + static_cast<double>(xs[mid - 1]));
}
}  // namespace

double OrderJourneyTracer::medianAckLatencyNs() const
{
  std::vector<int64_t> samples;
  samples.reserve(_byOrder.size());
  for (const auto& [_, trace] : _byOrder)
  {
    for (const auto& rec : trace)
    {
      if (rec.timestamps.acceptedAtNs > 0 && rec.timestamps.submittedAtNs > 0)
      {
        samples.push_back(rec.timestamps.acceptedAtNs - rec.timestamps.submittedAtNs);
        break;
      }
    }
  }
  return computeMedian(samples);
}

double OrderJourneyTracer::medianTimeToFirstFillNs() const
{
  std::vector<int64_t> samples;
  samples.reserve(_byOrder.size());
  for (const auto& [_, trace] : _byOrder)
  {
    for (const auto& rec : trace)
    {
      if (rec.timestamps.firstFillAtNs > 0 && rec.timestamps.submittedAtNs > 0)
      {
        samples.push_back(rec.timestamps.firstFillAtNs - rec.timestamps.submittedAtNs);
        break;
      }
    }
  }
  return computeMedian(samples);
}

double OrderJourneyTracer::makerFillRatio() const
{
  size_t fills = 0;
  size_t makers = 0;
  for (const auto& [_, trace] : _byOrder)
  {
    for (const auto& rec : trace)
    {
      // status 3 = PARTIALLY_FILLED, 4 = FILLED
      if (rec.status == 3 || rec.status == 4)
      {
        ++fills;
        if (rec.isMaker != 0)
        {
          ++makers;
        }
      }
    }
  }
  if (fills == 0)
  {
    return std::nan("");
  }
  return static_cast<double>(makers) / static_cast<double>(fills);
}

double OrderJourneyTracer::cancelRaceLossRate() const
{
  size_t cancelAttempts = 0;
  size_t lostToFill = 0;
  for (const auto& [_, trace] : _byOrder)
  {
    bool sawPendingCancel = false;
    bool sawLateCancelReject = false;
    for (const auto& rec : trace)
    {
      // status 5 = PENDING_CANCEL, 8 = REJECTED
      if (rec.status == 5)
      {
        sawPendingCancel = true;
      }
      // Reject reason carried alongside; lateness is encoded in
      // the rejected_at_ns + a fill being present. Approximate by
      // counting REJECTED that arrives after a fill within the same
      // trace.
      if (rec.status == 8 && sawPendingCancel)
      {
        sawLateCancelReject = true;
      }
    }
    if (sawPendingCancel)
    {
      ++cancelAttempts;
      if (sawLateCancelReject)
      {
        ++lostToFill;
      }
    }
  }
  if (cancelAttempts == 0)
  {
    return std::nan("");
  }
  return static_cast<double>(lostToFill) / static_cast<double>(cancelAttempts);
}

void OrderJourneyTracer::clear()
{
  _byOrder.clear();
  _insertionOrder.clear();
}

}  // namespace flox
