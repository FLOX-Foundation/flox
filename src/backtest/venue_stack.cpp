/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/venue_stack.h"

#include <cctype>

namespace flox
{

namespace
{

std::string toLower(const std::string& s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s)
  {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// Common wire-up shared across canned venues. Builds the executor +
// account peer relationships consistently: liquidation attached to
// account + executor, fees bound to account, queue/iceberg/rate-limit
// applied to executor, venue availability pointer installed.
VenueStack wireStack(uint64_t accountId, double equity,
                     std::unique_ptr<FeeSchedule> fees,
                     std::unique_ptr<FundingSchedule> funding,
                     std::unique_ptr<LiquidationEngine> liquidation,
                     RateLimitPolicy rateLimits, QueueModel queueModel,
                     size_t queueDepth, int64_t icebergRefreshLatencyNs,
                     const std::string& venueName)
{
  VenueStack::AssembleArgs a;
  a.clock = std::make_unique<SimulatedClock>();
  a.executor = std::make_unique<SimulatedExecutor>(*a.clock);
  a.account = std::make_unique<Account>(accountId, equity);
  a.account->setMarginMode(MarginMode::Cross);
  a.fees = std::move(fees);
  a.funding = std::move(funding);
  a.liquidation = std::move(liquidation);
  a.venue = std::make_unique<VenueAvailability>();
  a.rateLimits = std::move(rateLimits);
  a.venueName = venueName;

  // Wire the executor.
  a.executor->setQueueModel(queueModel, queueDepth);
  if (icebergRefreshLatencyNs > 0)
  {
    a.executor->setIcebergRefreshLatency(icebergRefreshLatencyNs);
  }
  a.executor->setVenueAvailability(a.venue.get());
  a.executor->setRateLimitPolicy(a.rateLimits);

  // Wire the account into liquidation + fees.
  a.fees->bindAccount(a.account.get());
  a.liquidation->attachAccount(a.account.get());
  a.liquidation->setExecutor(a.executor.get());

  return VenueStack::assemble(std::move(a));
}

}  // namespace

VenueStack VenueStack::assemble(AssembleArgs&& args)
{
  VenueStack s;
  s._clock = std::move(args.clock);
  s._executor = std::move(args.executor);
  s._account = std::move(args.account);
  s._liquidation = std::move(args.liquidation);
  s._fees = std::move(args.fees);
  s._funding = std::move(args.funding);
  s._venue = std::move(args.venue);
  s._rateLimits = std::move(args.rateLimits);
  s._venueName = std::move(args.venueName);
  return s;
}

VenueStack VenueStack::binance_um_futures(uint64_t accountId, double equity)
{
  // Binance USDT-margined linear futures defaults:
  // - FULL queue model with depth 8 (most realistic, enables T020
  //   live-queue calibration + iceberg refresh).
  // - 50 ms iceberg refresh latency (Binance typical).
  return wireStack(accountId, equity,
                   std::make_unique<FeeSchedule>(FeeSchedule::binance_um_futures()),
                   std::make_unique<FundingSchedule>(FundingSchedule::binance_um_futures()),
                   std::make_unique<LiquidationEngine>(LiquidationEngine::binance_um_futures()),
                   RateLimitPolicy::binance_um_futures(), QueueModel::FULL,
                   /*queueDepth=*/8, /*icebergRefreshLatencyNs=*/50'000'000LL,
                   "binance_um_futures");
}

VenueStack VenueStack::bybit_linear(uint64_t accountId, double equity)
{
  return wireStack(accountId, equity,
                   std::make_unique<FeeSchedule>(FeeSchedule::bybit_linear()),
                   std::make_unique<FundingSchedule>(FundingSchedule::bybit_linear()),
                   std::make_unique<LiquidationEngine>(LiquidationEngine::bybit_linear()),
                   RateLimitPolicy::bybit_linear(), QueueModel::FULL,
                   /*queueDepth=*/8, /*icebergRefreshLatencyNs=*/50'000'000LL,
                   "bybit_linear");
}

VenueStack VenueStack::okx_swap(uint64_t accountId, double equity)
{
  return wireStack(accountId, equity,
                   std::make_unique<FeeSchedule>(FeeSchedule::okx_swap()),
                   std::make_unique<FundingSchedule>(FundingSchedule::okx_swap()),
                   std::make_unique<LiquidationEngine>(LiquidationEngine::okx_swap()),
                   RateLimitPolicy::okx_swap(), QueueModel::FULL,
                   /*queueDepth=*/8, /*icebergRefreshLatencyNs=*/50'000'000LL,
                   "okx_swap");
}

VenueStack VenueStack::deribit(uint64_t accountId, double equity)
{
  // Deribit: pro-rata-with-FIFO is the dominant matching mode on
  // options + perp inverse. Funding schedule is hourly; no canned
  // factory exists yet so we use binance-style 8h as a placeholder
  // until a deribit-specific FundingSchedule lands.
  return wireStack(
      accountId, equity, std::make_unique<FeeSchedule>(FeeSchedule::deribit()),
      std::make_unique<FundingSchedule>(FundingSchedule::binance_um_futures()),
      // LiquidationEngine has no deribit profile yet; fall back to
      // bybit_linear which uses similar tier maths.
      std::make_unique<LiquidationEngine>(LiquidationEngine::bybit_linear()),
      RateLimitPolicy::deribit(), QueueModel::PRO_RATA_WITH_FIFO,
      /*queueDepth=*/8, /*icebergRefreshLatencyNs=*/50'000'000LL, "deribit");
}

VenueStack VenueStack::fromVenue(const std::string& name, uint64_t accountId,
                                 double equity)
{
  const std::string lower = toLower(name);
  if (lower == "binance_um_futures" || lower == "binance")
  {
    return binance_um_futures(accountId, equity);
  }
  if (lower == "bybit_linear" || lower == "bybit")
  {
    return bybit_linear(accountId, equity);
  }
  if (lower == "okx_swap" || lower == "okx")
  {
    return okx_swap(accountId, equity);
  }
  if (lower == "deribit")
  {
    return deribit(accountId, equity);
  }
  // Unknown venue: return an empty stack. Caller checks venueName().
  VenueStack empty;
  return empty;
}

}  // namespace flox
