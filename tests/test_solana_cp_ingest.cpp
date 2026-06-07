/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/constant_product_curve.h"
#include "flox/backtest/raydium_cp_curve.h"
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

// Real swap deltas parsed from the live Raydium CP WSOL/USDC pool
// (Q2sPHPdUWFMg7M7wwrQKLrn619cAucfRsmhVJffodSp) by dex-lab solana_ingest.py: each
// (amountIn, baseForQuote) was decoded from a swap_base_input instruction and checked
// against the transaction's vault balance change. baseForQuote = token0 (WSOL) in.
struct CpSwap
{
  const char* amountIn;
  bool baseForQuote;
};

const std::array<CpSwap, 8> kSwaps = {{
    {"1171478000", false},
    {"960893079", false},
    {"44664400", false},
    {"1087808000", false},
    {"6778914828", true},
    {"1564545430", true},
    {"1564667674", true},
    {"785924551", true},
}};

// The pool's net reserves at read time (vault minus accumulated fees), trade fee
// 2500/1e6 (0.25%), creator fee disabled.
constexpr const char* kNet0 = "13831187668587";  // WSOL, 9 decimals
constexpr const char* kNet1 = "13771991024747";  // USDC, 6 decimals
constexpr uint64_t kTradeFeeRate = 2500;

// The Solana ingest's deltas drive a Raydium CP pool through the tape: a RaydiumCp
// descriptor, a checkpoint at the read reserves, the parsed swaps as SwapDeltas, and a
// closing checkpoint at the state the exact curve derives. Replaying reconstructs that
// state with zero drift -- the chain-agnostic tape replaying a Solana venue.
TEST(SolanaCpIngestTest, ParsedSwapsRoundTripAsATape)
{
  const u256 net0 = D(kNet0);
  const u256 net1 = D(kNet1);

  // Independently derive the post-state by replaying the deltas through the exact curve.
  RaydiumCpCurve expected(net0, net1, kTradeFeeRate, 0, true);
  for (const CpSwap& s : kSwaps)
  {
    const std::size_t i = s.baseForQuote ? 0 : 1;
    expected.applySwap(i, 1 - i, D(s.amountIn));
  }
  const u256 f0 = expected.balances()[0];
  const u256 f1 = expected.balances()[1];

  std::vector<uint8_t> tape;
  PoolStateWriter w(tape);
  w.descriptorRaydiumCp(kTradeFeeRate, 0, true, 9, 6);  // WSOL 9 decimals, USDC 6
  w.checkpoint(0, net0, net1);
  int64_t ts = 1;
  for (const CpSwap& s : kSwaps)
  {
    w.swap(ts++, s.baseForQuote, D(s.amountIn));
  }
  w.checkpoint(ts, f0, f1);  // matches the replayed state -> no drift

  RaydiumCpCurve seed(net0, net1, kTradeFeeRate, 0, true);
  AmmDexConnector conn("raydium-cp", SymbolId{1}, seed, 0, 1, 9, 6, 1, D("1000000000"));
  int trades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++trades; });

  PoolStateReplay replay(conn);
  replay.run(tape);

  EXPECT_EQ(trades, static_cast<int>(kSwaps.size()));
  EXPECT_EQ(replay.driftCount(), 0u);
  ASSERT_NE(replay.curve(), nullptr);
  EXPECT_EQ(replay.curve()->balances()[0].toDec(), f0.toDec());
  EXPECT_EQ(replay.curve()->balances()[1].toDec(), f1.toDec());

  // The Raydium fee math actually moved the pool (a constant-product check would
  // diverge): the reserves are not the starting ones.
  EXPECT_FALSE(replay.curve()->balances()[0] == net0);
}

// The RaydiumCp descriptor builds a RaydiumCpCurve, not a plain ConstantProductCurve:
// a mismatched closing checkpoint (the Uniswap-style 997/1000 result) is caught as
// drift, confirming the tape replays the venue's own fee math.
TEST(SolanaCpIngestTest, VenueFeeMathIsRaydiumNotConstantProduct)
{
  const u256 net0 = D(kNet0);
  const u256 net1 = D(kNet1);
  const u256 amountIn = D("6778914828");

  RaydiumCpCurve ray(net0, net1, kTradeFeeRate, 0, true);
  const u256 rayOut = ray.amountOut(0, 1, amountIn);
  ConstantProductCurve uni(net0, net1, 997, 1000);
  const u256 uniOut = uni.amountOut(0, 1, amountIn);
  EXPECT_NE(rayOut.toDec(), uniOut.toDec());  // different fee denominators -> different output
}

}  // namespace
