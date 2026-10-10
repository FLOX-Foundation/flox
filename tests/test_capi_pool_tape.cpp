/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
std::string balance(LrvxCurveHandle c, size_t i)
{
  char out[96] = {0};
  EXPECT_EQ(lrvx_curve_balance(c, i, out, sizeof(out)), 1);
  return out;
}
}  // namespace

// Build a constant-product tape (descriptor + checkpoint + two swaps + a matching
// closing checkpoint), replay it, and confirm the replay reconstructs the exact state
// through the curve with zero drift -- a DEX backtest on recorded pool data from the
// C-ABI.
TEST(CapiPoolTapeTest, ConstantProductRoundTrip)
{
  const char* r0 = "1000000000000000000000";
  const char* r1 = "2000000000000000000000";
  // Final reserves after selling 5 base then buying 3 base back, computed by the curve
  // (the same math the replay uses); the closing checkpoint must match these.
  LrvxCurveHandle ref = lrvx_curve_constant_product(r0, r1, 997, 1000);
  char tmp[96] = {0};
  lrvx_curve_apply_swap(ref, 0, 1, "5000000000000000000", tmp, sizeof(tmp));
  lrvx_curve_apply_swap(ref, 1, 0, "3000000000000000000", tmp, sizeof(tmp));
  const std::string f0 = balance(ref, 0);
  const std::string f1 = balance(ref, 1);

  LrvxPoolTapeHandle tape = lrvx_pool_tape_create();
  lrvx_pool_tape_descriptor_constant_product(tape, 997, 1000, 18, 18);
  EXPECT_EQ(lrvx_pool_tape_checkpoint(tape, 100, r0, r1), 1);
  EXPECT_EQ(lrvx_pool_tape_swap(tape, 200, 1, "5000000000000000000"), 1);  // base in
  EXPECT_EQ(lrvx_pool_tape_swap(tape, 300, 0, "3000000000000000000"), 1);  // quote in
  EXPECT_EQ(lrvx_pool_tape_checkpoint(tape, 400, f0.c_str(), f1.c_str()), 1);

  LrvxPoolReplayHandle replay = lrvx_pool_tape_replay(tape, 0, 1, 18, 18);
  ASSERT_NE(replay, nullptr);
  EXPECT_EQ(lrvx_pool_replay_drift_count(replay), 0u);
  EXPECT_EQ(lrvx_pool_replay_trade_count(replay), 2u);

  LrvxCurveHandle curve = lrvx_pool_replay_curve(replay);
  ASSERT_NE(curve, nullptr);
  EXPECT_EQ(balance(curve, 0), f0);
  EXPECT_EQ(balance(curve, 1), f1);

  lrvx_pool_replay_destroy(replay);
  lrvx_pool_tape_destroy(tape);
  lrvx_curve_destroy(ref);
}

// A closing checkpoint that disagrees with the replayed state is counted as drift.
TEST(CapiPoolTapeTest, MismatchedCheckpointIsDrift)
{
  const char* r0 = "1000000000000000000000";
  const char* r1 = "2000000000000000000000";

  LrvxPoolTapeHandle tape = lrvx_pool_tape_create();
  lrvx_pool_tape_descriptor_constant_product(tape, 997, 1000, 18, 18);
  lrvx_pool_tape_checkpoint(tape, 100, r0, r1);
  lrvx_pool_tape_swap(tape, 200, 1, "5000000000000000000");
  lrvx_pool_tape_checkpoint(tape, 300, r0, r1);  // unchanged -> disagrees with the swap

  LrvxPoolReplayHandle replay = lrvx_pool_tape_replay(tape, 0, 1, 18, 18);
  EXPECT_EQ(lrvx_pool_replay_drift_count(replay), 1u);

  lrvx_pool_replay_destroy(replay);
  lrvx_pool_tape_destroy(tape);
}

TEST(CapiPoolTapeTest, RejectsBadAmount)
{
  LrvxPoolTapeHandle tape = lrvx_pool_tape_create();
  lrvx_pool_tape_descriptor_constant_product(tape, 997, 1000, 18, 18);
  EXPECT_EQ(lrvx_pool_tape_checkpoint(tape, 100, "not a number", "1"), 0);
  EXPECT_EQ(lrvx_pool_tape_swap(tape, 200, 1, "bad"), 0);
  lrvx_pool_tape_destroy(tape);
}
