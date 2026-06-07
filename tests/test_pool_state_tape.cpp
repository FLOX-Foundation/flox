/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/pool_state_tape.h"

#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/backtest/constant_product_curve.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

AmmDexConnector makeConnector(INTokenCurve& seed)
{
  return AmmDexConnector("amm", SymbolId{1}, seed, 0, 1, 18, 18, 1, D("1000000000000000000"));
}

// Write a descriptor + checkpoint + swaps + a matching checkpoint, then replay:
// the deltas reconstruct the exact state through the curve, the connector emits the
// trades, and the drift check passes because the final checkpoint matches.
TEST(PoolStateTapeTest, RoundTripReconstructsState)
{
  const u256 r0 = D("1000000000000000000000");
  const u256 r1 = D("2000000000000000000000");
  const u256 a1 = D("5000000000000000000");  // base -> quote
  const u256 a2 = D("3000000000000000000");  // quote -> base

  // Independently apply the swaps to get the expected final reserves.
  ConstantProductCurve expected(r0, r1, 997, 1000);
  expected.applySwap(0, 1, a1);
  expected.applySwap(1, 0, a2);
  const u256 f0 = expected.balances()[0];
  const u256 f1 = expected.balances()[1];

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorConstantProduct(997, 1000, 18, 18);
  w.checkpoint(100, r0, r1);
  w.swap(200, true, a1);
  w.swap(300, false, a2);
  w.checkpoint(400, f0, f1);  // matches the replayed state -> no drift

  ConstantProductCurve seed(r0, r1, 997, 1000);
  AmmDexConnector conn = makeConnector(seed);
  int trades = 0, books = 0;
  conn.setCallbacks([&](const BookUpdateEvent&)
                    { ++books; },
                    [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(replay.driftCount(), 0u);
  EXPECT_EQ(trades, 2);  // the two swaps
  EXPECT_GT(books, 0);   // checkpoints + swaps republish the book
  ASSERT_NE(replay.curve(), nullptr);
  EXPECT_EQ(replay.curve()->balances()[0].toDec(), f0.toDec());
  EXPECT_EQ(replay.curve()->balances()[1].toDec(), f1.toDec());
}

// The same delta-log replay works for a concentrated-liquidity venue: the
// checkpoint carries the sqrt price, liquidity, and tick array; a swap that crosses
// an initialized tick is reconstructed through the exact v3 curve; and the closing
// checkpoint matches the replayed sqrt-price / liquidity state, so no drift. This is
// the venue-shaped checkpoint and the generic balances() drift check.
TEST(PoolStateTapeTest, ClmmRoundTripCrossesTick)
{
  const u256 q96 = D("79228162514264337593543950336");
  const u256 sqrt0 = q96;                      // price 1.0
  const u256 L0 = D("1000000000000000000");    // 1e18
  const u256 amtIn = D("300000000000000000");  // token0 in, crosses the tick below
  std::vector<ClTick> ticks{{D("71305346262837903834189555302"), i256(D("500000000000000000"))}};

  // Independently apply the swap to get the expected post-state.
  ConcentratedLiquidityCurve expected(sqrt0, L0, 3000, ticks);
  const u256 outExpected = expected.amountOut(0, 1, amtIn);
  expected.applySwap(0, 1, amtIn);
  const u256 sqrt1 = expected.sqrtPrice();
  const u256 L1 = expected.liquidity();

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorClmm(PoolVenue::UniswapV3, 3000, 18, 18);
  w.checkpointClmm(100, sqrt0, L0, ticks);
  w.swap(200, true, amtIn);                 // token0 (base) in
  w.checkpointClmm(300, sqrt1, L1, ticks);  // matches replayed state -> no drift

  ConcentratedLiquidityCurve seed(sqrt0, L0, 3000, ticks);
  AmmDexConnector conn = makeConnector(seed);
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(replay.driftCount(), 0u);
  EXPECT_EQ(trades, 1);
  ASSERT_NE(replay.curve(), nullptr);
  const auto* cl = dynamic_cast<const ConcentratedLiquidityCurve*>(replay.curve());
  ASSERT_NE(cl, nullptr);
  EXPECT_EQ(cl->sqrtPrice().toDec(), sqrt1.toDec());
  EXPECT_EQ(cl->liquidity().toDec(), L1.toDec());
  // The cross-tick swap output is non-trivial (matches the curve's own quote).
  EXPECT_EQ(outExpected.toDec(), "213772620630911997");
}

// A checkpoint that disagrees with the replayed state is caught as drift, not
// silently carried -- the correctness guarantee for an unobserved mutation.
TEST(PoolStateTapeTest, MismatchedCheckpointIsDrift)
{
  const u256 r0 = D("1000000000000000000000");
  const u256 r1 = D("2000000000000000000000");

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorConstantProduct(997, 1000, 18, 18);
  w.checkpoint(100, r0, r1);
  w.swap(200, true, D("5000000000000000000"));
  // A checkpoint that does NOT match the post-swap state (a mutation we did not see).
  w.checkpoint(300, r0, r1);

  ConstantProductCurve seed(r0, r1, 997, 1000);
  AmmDexConnector conn = makeConnector(seed);
  conn.setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});

  PoolStateReplay replay(conn);
  replay.run(tape);
  EXPECT_EQ(replay.driftCount(), 1u);  // caught and re-anchored
}

// An unknown record type is skipped via its length frame (forward compatibility).
TEST(PoolStateTapeTest, UnknownRecordSkipped)
{
  const u256 r0 = D("1000000000000000000000");
  const u256 r1 = D("2000000000000000000000");
  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorConstantProduct(997, 1000, 18, 18);
  w.checkpoint(100, r0, r1);
  // Hand-write an unknown record type 99 with a 5-byte payload.
  tape.push_back(99);
  for (int i = 7; i >= 0; --i)
  {
    tape.push_back(static_cast<uint8_t>(static_cast<uint64_t>(5) >> (i * 8)));
  }
  for (int i = 0; i < 5; ++i)
  {
    tape.push_back(0xAB);
  }
  w.swap(200, true, D("5000000000000000000"));

  ConstantProductCurve seed(r0, r1, 997, 1000);
  AmmDexConnector conn = makeConnector(seed);
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });
  PoolStateReplay replay(conn);
  replay.run(tape);  // must not throw or desync on the unknown record
  EXPECT_EQ(trades, 1);
}

}  // namespace
