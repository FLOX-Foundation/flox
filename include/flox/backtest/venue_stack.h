/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/account.h"
#include "flox/backtest/fee_schedule.h"
#include "flox/backtest/funding_schedule.h"
#include "flox/backtest/liquidation_engine.h"
#include "flox/backtest/rate_limit_policy.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/backtest/venue_availability.h"
#include "flox/common.h"

#include <cstdint>
#include <memory>
#include <string>

namespace flox
{

// VenueStack — single-call wire-up for a venue-realistic backtest.
//
// Real prop accounts and exchange physics involve ~8 subsystems
// that must be assembled together: SimulatedClock, SimulatedExecutor,
// Account (cross-margin), LiquidationEngine, FeeSchedule,
// FundingSchedule, RateLimitPolicy, VenueAvailability. Plus tuning
// knobs: queue model, ack-latency profile, iceberg refresh.
//
// VenueStack bundles them with venue-realistic defaults so a
// research backtest starts from one factory call:
//
//   auto stack = flox::VenueStack::binance_um_futures(
//       /*account_id=*/42, /*equity=*/10'000.0);
//   stack.account().open_position(BTC, 1.0, 50'000.0);
//   stack.liquidation().on_mark(BTC, 47'000.0);
//
// Ownership: VenueStack owns every component. Accessors return
// references to the owned instances. The stack is move-only;
// pointers handed to peer subsystems (e.g. LiquidationEngine's
// pointer to Account, Executor's pointer to VenueAvailability)
// remain valid for the stack's lifetime.
//
// The bare `SimulatedExecutor()` constructor remains available for
// unit tests of the executor itself; for backtests of real
// strategies always go through a venue factory below.
class VenueStack
{
 public:
  // Canned-venue factories. Equity is the account's starting
  // margin pool (USDT for linear venues). account_id is opaque to
  // the engine, used only for STP and stats correlation.
  static VenueStack binance_um_futures(uint64_t accountId, double equity);
  static VenueStack bybit_linear(uint64_t accountId, double equity);
  static VenueStack okx_swap(uint64_t accountId, double equity);
  static VenueStack deribit(uint64_t accountId, double equity);

  // String-based dispatcher for codegen / AI agents that build
  // the call from data. Accepts "binance_um_futures",
  // "bybit_linear", "okx_swap", "deribit" (case-insensitive).
  // Throws / returns empty stack on unknown name; check via
  // `venueName()`.
  static VenueStack fromVenue(const std::string& name, uint64_t accountId,
                              double equity);

  // Non-owning accessors. The returned references live as long as
  // `*this`.
  SimulatedClock& clock() noexcept { return *_clock; }
  SimulatedExecutor& executor() noexcept { return *_executor; }
  Account& account() noexcept { return *_account; }
  LiquidationEngine& liquidation() noexcept { return *_liquidation; }
  FeeSchedule& fees() noexcept { return *_fees; }
  FundingSchedule& funding() noexcept { return *_funding; }
  VenueAvailability& venue() noexcept { return *_venue; }
  // RateLimitPolicy is owned here but already attached to the
  // executor; expose it for inspection / tuning.
  const RateLimitPolicy& rateLimits() const noexcept { return _rateLimits; }

  // Name of the canned venue this stack was built from. Empty if
  // constructed by the assemble() escape hatch.
  const std::string& venueName() const noexcept { return _venueName; }

  // Escape hatch for custom venues. Caller passes already-built
  // subsystems; VenueStack takes ownership and wires the
  // peer-pointer relationships consistently with the canned path.
  // Callers populating this must ensure subsystems aren't already
  // wired elsewhere.
  struct AssembleArgs
  {
    std::unique_ptr<SimulatedClock> clock;
    std::unique_ptr<SimulatedExecutor> executor;
    std::unique_ptr<Account> account;
    std::unique_ptr<LiquidationEngine> liquidation;
    std::unique_ptr<FeeSchedule> fees;
    std::unique_ptr<FundingSchedule> funding;
    std::unique_ptr<VenueAvailability> venue;
    RateLimitPolicy rateLimits;
    std::string venueName;
  };
  static VenueStack assemble(AssembleArgs&& args);

  VenueStack(const VenueStack&) = delete;
  VenueStack& operator=(const VenueStack&) = delete;
  VenueStack(VenueStack&&) = default;
  VenueStack& operator=(VenueStack&&) = default;
  ~VenueStack() = default;

 private:
  VenueStack() = default;

  std::unique_ptr<SimulatedClock> _clock;
  std::unique_ptr<SimulatedExecutor> _executor;
  std::unique_ptr<Account> _account;
  std::unique_ptr<LiquidationEngine> _liquidation;
  std::unique_ptr<FeeSchedule> _fees;
  std::unique_ptr<FundingSchedule> _funding;
  std::unique_ptr<VenueAvailability> _venue;
  RateLimitPolicy _rateLimits;
  std::string _venueName;
};

}  // namespace flox
