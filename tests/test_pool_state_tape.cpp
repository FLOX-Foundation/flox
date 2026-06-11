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
#include "flox/backtest/cryptoswap_curve.h"
#include "flox/backtest/meteora_dlmm_curve.h"
#include "flox/backtest/stableswap_curve.h"
#include "flox/backtest/weighted_curve.h"

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

// A balances-shaped venue round-trips: the descriptor carries the StableSwap
// parameters, a checkpoint carries the full 3-coin balance vector, and an n-token
// SwapDelta names its (i, j) pair. The (0,1) swap is the connector's pair and
// prints a trade; the (1,2) swap is off-pair -- the shared state still moves and
// the book republishes, but no trade prints on this symbol. The closing checkpoint
// matches the replayed balances across all three coins, so no drift.
TEST(PoolStateTapeTest, StableSwapRoundTripOffPairSwapMovesState)
{
  const std::vector<u256> rates{u256::pow10(18), u256::pow10(30), u256::pow10(30)};
  const u256 fee = D("1500000");
  const std::vector<u256> bal{D("50000000000000000000000000"),  // 50M DAI
                              D("50000000000000"),              // 50M USDC
                              D("50000000000000")};             // 50M USDT
  const u256 dxPair = D("1000000000000000000000000");           // 1M DAI -> USDC
  const u256 dxOff = D("2000000000000");                        // 2M USDC -> USDT

  StableSwapCurve expected(bal, rates, 4000, fee);
  expected.applySwap(0, 1, dxPair);
  expected.applySwap(1, 2, dxOff);

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorStableSwap(rates, 4000, fee, 18, 6);
  w.checkpointBalances(100, bal);
  w.swapN(200, 0, 1, dxPair);
  w.swapN(300, 1, 2, dxOff);
  w.checkpointBalances(400, expected.balances());

  StableSwapCurve seed(bal, rates, 4000, fee);
  AmmDexConnector conn("amm", SymbolId{1}, seed, 0, 1, 18, 6, 1, D("1000000000000000000"));
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(replay.driftCount(), 0u);
  EXPECT_EQ(trades, 1);  // the off-pair swap moves state without a trade print
  ASSERT_NE(replay.curve(), nullptr);
  for (std::size_t k = 0; k < 3; ++k)
  {
    EXPECT_EQ(replay.curve()->balances()[k].toDec(), expected.balances()[k].toDec());
  }
}

// A weighted venue replays through the directional SwapDelta: the 80/20 pool's
// exact Balancer math reconstructs the post-state, and the balances-shaped
// checkpoint anchors it.
TEST(PoolStateTapeTest, WeightedRoundTripDirectionalSwap)
{
  const std::vector<u256> sf{u256::pow10(18), u256::pow10(18)};
  const std::vector<u256> weights{D("800000000000000000"), D("200000000000000000")};
  const u256 fee = D("10000000000000000");
  const std::vector<u256> bal{D("1000000000000000000000000"), D("1000000000000000000000")};
  const u256 dx = D("10000000000000000000000");  // BAL -> WETH

  WeightedCurve expected(bal, sf, weights, fee);
  expected.applySwap(0, 1, dx);

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorWeighted(sf, weights, fee, 18, 18);
  w.checkpointBalances(100, bal);
  w.swap(200, true, dx);
  w.checkpointBalances(300, expected.balances());

  WeightedCurve seed(bal, sf, weights, fee);
  AmmDexConnector conn = makeConnector(seed);
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(replay.driftCount(), 0u);
  EXPECT_EQ(trades, 1);
  ASSERT_NE(replay.curve(), nullptr);
  EXPECT_EQ(replay.curve()->balances()[0].toDec(), expected.balances()[0].toDec());
  EXPECT_EQ(replay.curve()->balances()[1].toDec(), expected.balances()[1].toDec());
}

// A cryptoswap venue carries its price scale in the checkpoint -- it is state the
// chain repegs, not a static parameter -- and the replayed curve quotes with the
// re-anchored scale. Tricrypto snapshot parameters, USDT/WBTC as the pair.
TEST(PoolStateTapeTest, CryptoswapRoundTripReanchorsPriceScale)
{
  const std::vector<u256> bal{D("3142759669571"), D("5014135703"),
                              D("1929303558813993742375")};
  const std::vector<u256> prec{u256::pow10(12), u256::pow10(10), u256(1)};
  const std::vector<u256> scale{D("61120002629999063359445"), D("1581037626232863066025")};
  const u256 gamma = D("11809167828997");
  const u256 midFee = D("3000000"), outFee = D("30000000"), feeGamma = D("500000000000000");
  const u256 dx = D("10000000000");  // 10,000 USDT -> WBTC

  CryptoswapCurve expected(bal, prec, scale, 1707629, gamma, midFee, outFee, feeGamma);
  expected.applySwap(0, 1, dx);

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorCryptoswap(prec, 1707629, gamma, midFee, outFee, feeGamma, 6, 8);
  w.checkpointCryptoswap(100, bal, scale);
  w.swapN(200, 0, 1, dx);
  w.checkpointCryptoswap(300, expected.balances(), scale);

  CryptoswapCurve seed(bal, prec, scale, 1707629, gamma, midFee, outFee, feeGamma);
  AmmDexConnector conn("amm", SymbolId{1}, seed, 0, 1, 6, 8, 1, D("1000000"));
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(replay.driftCount(), 0u);
  EXPECT_EQ(trades, 1);
  ASSERT_NE(replay.curve(), nullptr);
  const auto* cs = dynamic_cast<const CryptoswapCurve*>(replay.curve());
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->priceScale()[0].toDec(), scale[0].toDec());
  for (std::size_t k = 0; k < 3; ++k)
  {
    EXPECT_EQ(cs->balances()[k].toDec(), expected.balances()[k].toDec());
  }
}

// A Meteora DLMM venue: the checkpoint carries the bin book and volatility state,
// a swap walks bins through the exact Liquidity Book math (moving the active bin
// and the bin reserves), and the closing checkpoint -- captured off the expected
// post-state -- matches, so no drift.
TEST(PoolStateTapeTest, DlmmRoundTripWalksBins)
{
  DlmmFeeParams params;
  params.baseFactor = 10000;
  params.baseFeePowerFactor = 0;
  params.variableFeeControl = 40000;
  params.filterPeriod = 30;
  params.decayPeriod = 600;
  params.reductionFactor = 5000;
  params.maxVolatilityAccumulator = 350000;
  std::vector<DlmmBin> bins;
  for (int32_t i = -3; i <= 3; ++i)
  {
    bins.push_back({i, D("1000000000"), D("1000000000")});
  }
  const u256 amtIn = D("2500000000");  // X in, crosses several bins downward

  MeteoraDlmmCurve expected(0, 10, bins, params, DlmmVolatility{}, 1000000);
  const u256 outExpected = expected.amountOut(0, 1, amtIn);
  expected.applySwap(0, 1, amtIn);
  EXPECT_LT(expected.activeId(), 0);  // the X-in walk moved the active bin down

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorDlmm(10, params, 9, 9);
  w.checkpointDlmm(100, 0, 1000000, DlmmVolatility{}, bins);
  w.swap(200, true, amtIn);
  w.checkpointDlmm(300, expected.activeId(), expected.timestamp(), expected.volatility(),
                   expected.bins());

  MeteoraDlmmCurve seed(0, 10, bins, params, DlmmVolatility{}, 1000000);
  AmmDexConnector conn("amm", SymbolId{1}, seed, 0, 1, 9, 9, 1, D("1000000000"));
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(replay.driftCount(), 0u);
  EXPECT_EQ(trades, 1);
  ASSERT_NE(replay.curve(), nullptr);
  const auto* dlmm = dynamic_cast<const MeteoraDlmmCurve*>(replay.curve());
  ASSERT_NE(dlmm, nullptr);
  EXPECT_EQ(dlmm->activeId(), expected.activeId());
  EXPECT_EQ(dlmm->balances()[0].toDec(), expected.balances()[0].toDec());
  EXPECT_EQ(dlmm->balances()[1].toDec(), expected.balances()[1].toDec());
  EXPECT_EQ(outExpected.toDec(), "2495496549");  // the program-reference output
}

// A balances-shaped checkpoint that disagrees with the replayed state is drift,
// the same guarantee the two-reserve venues have.
TEST(PoolStateTapeTest, StableSwapMismatchedCheckpointIsDrift)
{
  const std::vector<u256> rates{u256::pow10(18), u256::pow10(30), u256::pow10(30)};
  const u256 fee = D("1500000");
  const std::vector<u256> bal{D("50000000000000000000000000"), D("50000000000000"),
                              D("50000000000000")};

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorStableSwap(rates, 4000, fee, 18, 6);
  w.checkpointBalances(100, bal);
  w.swapN(200, 0, 1, D("1000000000000000000000000"));
  w.checkpointBalances(300, bal);  // does NOT match the post-swap state

  StableSwapCurve seed(bal, rates, 4000, fee);
  AmmDexConnector conn("amm", SymbolId{1}, seed, 0, 1, 18, 6, 1, D("1000000000000000000"));
  conn.setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});

  PoolStateReplay replay(conn);
  replay.run(tape);
  EXPECT_EQ(replay.driftCount(), 1u);
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
