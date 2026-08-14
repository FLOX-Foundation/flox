/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/ledger.h"
#include "flox-venue/messages.h"
#include "flox-venue/reject_reason.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace flox::venue
{

class LatencyHistogram
{
 public:
  static constexpr int kBuckets = 64;  // bucket b covers [2^(b-1), 2^b) ns

  void record(uint64_t ns) noexcept
  {
    int b = (ns == 0) ? 0 : (64 - __builtin_clzll(ns));
    if (b >= kBuckets)
    {
      b = kBuckets - 1;
    }
    ++buckets_[static_cast<size_t>(b)];
    ++count_;
    sumNs_ += ns;
    if (ns > maxNs_)
    {
      maxNs_ = ns;
    }
  }

  uint64_t count() const noexcept { return count_; }
  uint64_t maxNs() const noexcept { return maxNs_; }
  double meanNs() const noexcept { return count_ ? static_cast<double>(sumNs_) / count_ : 0.0; }

  // Upper-bound (ns) of the bucket holding the p-th percentile [0,1].
  uint64_t percentileNs(double p) const noexcept
  {
    if (count_ == 0)
    {
      return 0;
    }
    const uint64_t target = static_cast<uint64_t>(static_cast<double>(count_) * p);
    uint64_t cum = 0;
    for (int b = 0; b < kBuckets; ++b)
    {
      cum += buckets_[static_cast<size_t>(b)];
      if (cum >= target && buckets_[static_cast<size_t>(b)] > 0)
      {
        return (b == 0) ? 1 : (1ULL << b);
      }
    }
    return maxNs_;
  }

 private:
  std::array<uint64_t, kBuckets> buckets_{};
  uint64_t count_{0};
  uint64_t sumNs_{0};
  uint64_t maxNs_{0};
};

struct Metrics
{
  LatencyHistogram submitLatency;
  uint64_t accepted{0};
  uint64_t trades{0};
  uint64_t cancels{0};
  uint64_t rejects{0};
  uint64_t modifies{0};
  uint64_t liquidations{0};
  uint64_t holds{0};
  __int128 volumeRaw{0};  // cumulative traded notional (quote raw, kMoneyScale)
  static constexpr size_t kReasons = 24;
  std::array<uint64_t, kReasons> rejectsByReason{};  // indexed by RejectReason

  // Symbols with a non-default scale must be registered so trade notional is
  // normalized to kMoneyScale; unregistered symbols use the default 1e8.
  void setSymbolScales(SymbolId s, int64_t priceScale, int64_t qtyScale)
  {
    scales_[s] = {priceScale, qtyScale};
  }

  void observe(const OutboundEvent& e) noexcept;

 private:
  std::unordered_map<SymbolId, std::pair<int64_t, int64_t>> scales_;
};

// Perimeter (gateway/delivery) counters. Atomics: they are bumped from
// connection reader threads, per-session writer threads and the matching
// thread concurrently, unlike Metrics which lives on the engine thread.
struct GatewayCounters
{
  std::atomic<uint64_t> slowConsumerDisconnects{0};  // outbound queue overflow -> session dropped
  std::atomic<uint64_t> idleDisconnects{0};          // liveness timeout -> session dropped
  std::atomic<uint64_t> resendServed{0};             // ResendRequest served from the session log
};

// Market-data feed counters. Atomics: sendDrops is bumped on the matching
// thread (publish path), the recovery counters on MdRecoveryServer connection
// threads.
struct MdCounters
{
  std::atomic<uint64_t> sendDrops{0};        // sendto failed/partial: datagram never left the host
  std::atomic<uint64_t> resendServed{0};     // recovery requests answered by ring replay
  std::atomic<uint64_t> snapshotsServed{0};  // recovery requests answered by a full snapshot
};

// Point-in-time venue state that is NOT derivable from the outbound event
// stream. The library does not sample these itself -- it has no monitoring
// thread -- so a deployment must populate a Gauges from its ledger/risk state
// and feed it to the Prometheus exporter (see the sampler in
// venue/tests for the shape). Left unsampled, every fme_* gauge reads zero.
// Zero is otherwise a valid, meaningful value here.
struct Gauges
{
  __int128 insuranceFundRaw{0};    // venue collateral balance (insurance fund)
  double fundingRate{0.0};         // last settled funding rate
  __int128 openInterestRaw{0};     // aggregate open position notional (quote raw)
  uint64_t openPositions{0};       // count of open perp positions
  uint64_t restingOrders{0};       // live orders across the book
  int64_t markPriceAgeNs{0};       // age of the current mark/index (feed-lag alert)
  uint64_t liquidationsPaused{0};  // 1 = liquidation circuit breaker engaged
};

inline void Metrics::observe(const OutboundEvent& e) noexcept
{
  if (const auto* t = std::get_if<Trade>(&e))
  {
    ++trades;
    auto it = scales_.find(t->symbol);
    const auto [pS, qS] = it == scales_.end()
                              ? std::pair<int64_t, int64_t>{Price::Scale, Quantity::Scale}
                              : it->second;
    volumeRaw += notionalRaw(t->price.raw(), t->quantity.raw(), pS, qS);
  }
  else if (std::get_if<OrderAccepted>(&e))
  {
    ++accepted;
  }
  else if (std::get_if<OrderCanceled>(&e))
  {
    ++cancels;
  }
  else if (const auto* r = std::get_if<OrderRejected>(&e))
  {
    ++rejects;
    const size_t idx = static_cast<size_t>(r->reason);
    if (idx < kReasons)
    {
      ++rejectsByReason[idx];
    }
  }
  else if (std::get_if<OrderModified>(&e))
  {
    ++modifies;
  }
  else if (std::get_if<Liquidation>(&e))
  {
    ++liquidations;
  }
  else if (std::get_if<FillHeld>(&e))
  {
    ++holds;
  }
}

}  // namespace flox::venue
