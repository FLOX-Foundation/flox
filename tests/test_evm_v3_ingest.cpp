/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/replay/pool_state_tape.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

// Six consecutive exact-input swaps on the live Uniswap v3 USDC/WETH 0.05% pool
// (0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640), captured from the Swap event logs
// via dex-lab evm_logs.py --v3. Each swap's logged post-state is the next swap's
// pre-state, and the active liquidity is unchanged across all six, so they stayed in
// one tick range and need no tick data. token0 is USDC (zeroForOne), token1 is WETH.
// This is the V3 ingest's historical 0-wei cross-check, frozen as a deterministic
// regression: the exact curve must reproduce each log's post sqrtPriceX96 and output.
struct V3Swap
{
  const char* preSqrt;
  const char* amountIn;
  const char* postSqrt;
  const char* amountOut;
};

constexpr const char* kLiquidity = "2620107401555790298";

const std::array<V3Swap, 6> kSwaps = {{
    {"1959671565952912287364002783533932", "11123069048", "1959465915458223185312768642070809",
     "6800945095394772678"},
    {"1959465915458223185312768642070809", "20249809", "1959465541106019720641379844276379",
     "12379978885789664"},
    {"1959465541106019720641379844276379", "3179565546", "1959406763196474932604186130694755",
     "1943809258715910742"},
    {"1959406763196474932604186130694755", "100000000", "1959404914639177023801042615188143",
     "61132538036317729"},
    {"1959404914639177023801042615188143", "9955333410", "1959220902053222089111955899494335",
     "6085370695718730815"},
    {"1959220902053222089111955899494335", "19980000", "1959220532781260294314317603129883",
     "12211973237564112"},
}};

// The exact v3 curve reproduces every logged post sqrtPriceX96 and output to the wei,
// swap after swap, off the first pre-state -- the chained on-chain cross-check.
TEST(EvmV3IngestTest, ChainedSwapsReproducePostStateToTheWei)
{
  ConcentratedLiquidityCurve pool(D(kSwaps[0].preSqrt), D(kLiquidity), 500, {});
  for (const V3Swap& s : kSwaps)
  {
    EXPECT_EQ(pool.sqrtPrice().toDec(), D(s.preSqrt).toDec());  // post of the previous swap
    const u256 out = pool.applySwap(0, 1, D(s.amountIn));       // token0 (USDC) in
    EXPECT_EQ(pool.sqrtPrice().toDec(), D(s.postSqrt).toDec());
    EXPECT_EQ(out.toDec(), D(s.amountOut).toDec());
  }
}

// The same decoded swaps written as a pool-state tape and replayed: a CLMM descriptor,
// a checkpoint at the pre-state, the swaps as SwapDeltas, and a closing checkpoint at
// the logged final state -- the replay reconstructs the exact pool with zero drift.
TEST(EvmV3IngestTest, DecodedSwapsRoundTripAsATape)
{
  const u256 liq = D(kLiquidity);
  const u256 pre0 = D(kSwaps[0].preSqrt);
  const u256 postN = D(kSwaps.back().postSqrt);

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorClmm(PoolVenue::UniswapV3, 500, 6, 18);  // USDC 6 decimals, WETH 18
  w.checkpointClmm(0, pre0, liq, {});
  int64_t ts = 1;
  for (const V3Swap& s : kSwaps)
  {
    w.swap(ts++, true, D(s.amountIn));  // token0 in
  }
  w.checkpointClmm(ts, postN, liq, {});  // matches the replayed state -> no drift

  ConcentratedLiquidityCurve seed(pre0, liq, 500, {});
  AmmDexConnector conn("uniswap-v3", SymbolId{1}, seed, 0, 1, 6, 18, 1, D("1000000000"));
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(trades, static_cast<int>(kSwaps.size()));
  EXPECT_EQ(replay.driftCount(), 0u);
  ASSERT_NE(replay.curve(), nullptr);
  const auto* cl = dynamic_cast<const ConcentratedLiquidityCurve*>(replay.curve());
  ASSERT_NE(cl, nullptr);
  EXPECT_EQ(cl->sqrtPrice().toDec(), postN.toDec());
}

}  // namespace
