/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The venue stack accessors, which had no C-level test at all.
//
// Two things are checked here. The executor accessor used to return the
// engine object where the header promised a handle, so every
// flox_simulated_executor_* call read the struct sixteen bytes off: fill
// counts came back as billions, advancing the clock overwrote the executor's
// own first sixteen bytes, and submitting crashed. And every accessor hands
// back a pointer the stack still owns, so a binding that wraps it in a
// finaliser destroyed the stack's member and then the stack destroyed it
// again.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

TEST(CapiVenueStackTest, ExecutorAccessorReturnsAUsableExecutorHandle)
{
  FloxVenueStackHandle stack = flox_venue_stack_create(/*venue=binance_um=*/0, 42, 10000.0);
  ASSERT_NE(stack, nullptr);

  FloxSimulatedExecutorHandle exec = flox_venue_stack_executor(stack);
  ASSERT_NE(exec, nullptr);

  EXPECT_EQ(flox_simulated_executor_fill_count(exec), 0u);

  flox_simulated_executor_on_best_levels(exec, 1, 100.0, 10.0, 100.0, 10.0);
  flox_simulated_executor_submit_order(exec, 1, /*buy=*/0, 0.0, 1.0, /*type=market=*/1, 1);
  EXPECT_EQ(flox_simulated_executor_fill_count(exec), 1u);

  FloxFill fill{};
  ASSERT_EQ(flox_simulated_executor_get_fills(exec, &fill, 1), 1u);
  EXPECT_EQ(fill.price_raw, 100LL * 100000000LL);

  flox_simulated_executor_advance_clock(exec, 1000);
  EXPECT_EQ(flox_simulated_executor_fill_count(exec), 1u);

  flox_simulated_executor_cancel_all(exec, 1);

  flox_venue_stack_destroy(stack);
}

TEST(CapiVenueStackTest, ExecutorAccessorIsStableAcrossCalls)
{
  FloxVenueStackHandle stack = flox_venue_stack_create(0, 7, 1000.0);
  ASSERT_NE(stack, nullptr);

  FloxSimulatedExecutorHandle first = flox_venue_stack_executor(stack);
  FloxSimulatedExecutorHandle second = flox_venue_stack_executor(stack);
  EXPECT_EQ(first, second);

  flox_venue_stack_destroy(stack);
}

TEST(CapiVenueStackTest, AccountAccessorReadsTheStacksAccount)
{
  FloxVenueStackHandle stack = flox_venue_stack_create(0, 99, 2500.0);
  ASSERT_NE(stack, nullptr);

  FloxAccountHandle account = flox_venue_stack_account(stack);
  ASSERT_NE(account, nullptr);
  EXPECT_EQ(flox_account_id(account), 99u);
  EXPECT_NEAR(flox_account_equity(account), 2500.0, 1e-9);

  flox_venue_stack_destroy(stack);
}

// Destroying a borrowed handle must be a no-op, not a free of a member the
// stack is about to free again. Before this, the first delete looked healthy
// and the abort landed later, inside flox_venue_stack_destroy.
TEST(CapiVenueStackTest, DestroyingABorrowedHandleIsANoOp)
{
  FloxVenueStackHandle stack = flox_venue_stack_create(0, 42, 10000.0);
  ASSERT_NE(stack, nullptr);

  FloxAccountHandle account = flox_venue_stack_account(stack);
  FloxLiquidationEngineHandle liquidation = flox_venue_stack_liquidation(stack);
  FloxFeeScheduleHandle fees = flox_venue_stack_fees(stack);
  FloxFundingScheduleHandle funding = flox_venue_stack_funding(stack);
  FloxVenueAvailabilityHandle availability = flox_venue_stack_venue(stack);
  FloxSimulatedExecutorHandle exec = flox_venue_stack_executor(stack);

  flox_account_destroy(account);
  flox_liquidation_engine_destroy(liquidation);
  flox_fee_schedule_destroy(fees);
  flox_funding_schedule_destroy(funding);
  flox_venue_availability_destroy(availability);
  flox_simulated_executor_destroy(exec);

  // The stack still owns every one of them.
  EXPECT_EQ(flox_account_id(account), 42u);
  EXPECT_EQ(flox_simulated_executor_fill_count(exec), 0u);

  flox_venue_stack_destroy(stack);
}

TEST(CapiVenueStackTest, VenueNameSurvivesTheAccessor)
{
  FloxVenueStackHandle stack = flox_venue_stack_create(1, 1, 100.0);
  ASSERT_NE(stack, nullptr);
  const char* name = flox_venue_stack_venue_name(stack);
  ASSERT_NE(name, nullptr);
  EXPECT_GT(std::string(name).size(), 0u);
  flox_venue_stack_destroy(stack);
}

// The venue stack is not the only composite that hands out a view into its
// own memory. These two were found by the JavaScript engine's audit of the
// same question, from the other side of the boundary: it had to decide, for
// every handle it wraps, whether its finaliser owns the memory. Its answer
// was seven borrowed views, against the six the venue stack has. The extra
// one is the recorder view below; the pool replay curve is an eighth that
// falls outside the JS surface because that binding has no pool tape yet.
//
// Both were documented as "do not destroy this" in the header and nowhere
// enforced, which is exactly the shape of the venue stack defect.

TEST(CapiVenueStackTest, DestroyingTheRecorderViewOfAHookIsANoOp)
{
  auto dir = std::filesystem::temp_directory_path() / "flox_capi_recorder_view";
  std::filesystem::create_directories(dir);

  FloxBinaryLogRecorderHookHandle hook =
      flox_binary_log_recorder_hook_create(dir.string().c_str(), 16, 0, 0);
  ASSERT_NE(hook, nullptr);

  FloxMarketDataRecorderHandle view = flox_binary_log_recorder_hook_as_recorder(hook);
  ASSERT_NE(view, nullptr);

  flox_market_data_recorder_destroy(view);

  // The hook still owns it, and destroying the hook must not double free.
  EXPECT_EQ(flox_binary_log_recorder_hook_as_recorder(hook), view);
  flox_binary_log_recorder_hook_destroy(hook);

  std::filesystem::remove_all(dir);
}

TEST(CapiVenueStackTest, DestroyingAPoolReplaysCurveIsANoOp)
{
  const char* r0 = "1000000000000000000000";
  const char* r1 = "2000000000000000000000";

  FloxPoolTapeHandle tape = flox_pool_tape_create();
  ASSERT_NE(tape, nullptr);
  flox_pool_tape_descriptor_constant_product(tape, 997, 1000, 18, 18);
  ASSERT_EQ(flox_pool_tape_checkpoint(tape, 100, r0, r1), 1);

  FloxPoolReplayHandle replay = flox_pool_tape_replay(tape, 0, 1, 18, 18);
  ASSERT_NE(replay, nullptr);

  FloxCurveHandle curve = flox_pool_replay_curve(replay);
  ASSERT_NE(curve, nullptr);

  flox_curve_destroy(curve);

  char out[96] = {0};
  EXPECT_EQ(flox_curve_balance(curve, 0, out, sizeof(out)), 1);

  flox_pool_replay_destroy(replay);
  flox_pool_tape_destroy(tape);
}

// A registration that outlived its owner would be worse than the defect it
// fixes: the allocator hands the address to a genuinely owned handle later,
// and _destroy on that one gets refused, so the caller cannot free it at all.
// The register/unregister pair runs with the composite's lifetime, so this
// walks a few hundred rounds and insists every owned handle still dies.
TEST(CapiVenueStackTest, RegistrationsDoNotOutliveTheirOwner)
{
  for (int round = 0; round < 512; ++round)
  {
    FloxVenueStackHandle stack = flox_venue_stack_create(0, 1, 1000.0);
    ASSERT_NE(stack, nullptr);
    FloxAccountHandle borrowed = flox_venue_stack_account(stack);
    ASSERT_NE(borrowed, nullptr);
    flox_venue_stack_destroy(stack);

    // Owned, and possibly at the address the stack's member just vacated.
    FloxAccountHandle owned = flox_account_create(7, 500.0);
    ASSERT_NE(owned, nullptr);
    EXPECT_EQ(flox_account_id(owned), 7u);
    flox_account_destroy(owned);
    // Refusing to destroy this one would show up as a leak in the tooling
    // and as a rising handle count here; reading it back after the destroy
    // is what the sanitizers watch.
  }
}
