/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/constant_product_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/connector/reorg_safe_pool_feed.h"

#include <gtest/gtest.h>

#include <memory>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

AmmDexConnector makeConn(ConstantProductCurve& seed)
{
  return AmmDexConnector("amm", SymbolId{1}, seed, 0, 1, 18, 18, 1, D("1000000000000000000"));
}

const u256 R0 = D("1000000000000000000000");
const u256 R1 = D("2000000000000000000000");

// With no reorg, the live feed's working curve is exactly what a plain replay of the
// same deltas produces -- a strategy sees an identical book live and from a tape.
TEST(ReorgSafePoolFeedTest, NoReorgMatchesPlainReplay)
{
  ConstantProductCurve expected(R0, R1, 997, 1000);
  expected.applySwap(0, 1, D("5000000000000000000"));
  expected.applySwap(1, 0, D("3000000000000000000"));

  ConstantProductCurve seed(R0, R1, 997, 1000);
  AmmDexConnector conn = makeConn(seed);
  conn.setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});
  ReorgSafePoolFeed feed(conn, std::make_unique<ConstantProductCurve>(R0, R1, 997, 1000));

  feed.onSwap(1, true, D("5000000000000000000"));
  feed.onSwap(2, false, D("3000000000000000000"));
  feed.finalize(2);

  ASSERT_NE(feed.curve(), nullptr);
  EXPECT_EQ(feed.curve()->balances()[0].toDec(), expected.balances()[0].toDec());
  EXPECT_EQ(feed.curve()->balances()[1].toDec(), expected.balances()[1].toDec());
  EXPECT_EQ(feed.pendingCount(), 0u);  // all finalised
}

// A reorg drops the optimistic swaps above the rolled-back height and rebuilds the
// working curve -- the state is as if those blocks never happened.
TEST(ReorgSafePoolFeedTest, ReorgDropsUnfinalizedSwaps)
{
  // The reference state after only the two surviving swaps (heights 1 and 2).
  ConstantProductCurve survived(R0, R1, 997, 1000);
  survived.applySwap(0, 1, D("5000000000000000000"));
  survived.applySwap(0, 1, D("1000000000000000000"));

  ConstantProductCurve seed(R0, R1, 997, 1000);
  AmmDexConnector conn = makeConn(seed);
  conn.setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});
  ReorgSafePoolFeed feed(conn, std::make_unique<ConstantProductCurve>(R0, R1, 997, 1000));

  feed.onSwap(1, true, D("5000000000000000000"));
  feed.onSwap(2, true, D("1000000000000000000"));
  feed.onSwap(3, true, D("9000000000000000000"));  // block 3 will be reorged away
  EXPECT_EQ(feed.pendingCount(), 3u);

  feed.reorg(2);  // roll back to height 2: block 3 did not stick
  EXPECT_EQ(feed.pendingCount(), 2u);
  EXPECT_EQ(feed.curve()->balances()[0].toDec(), survived.balances()[0].toDec());
  EXPECT_EQ(feed.curve()->balances()[1].toDec(), survived.balances()[1].toDec());

  // A new block 3 then arrives with a different swap; the state follows the new fork.
  feed.onSwap(3, false, D("4000000000000000000"));
  ConstantProductCurve newFork(R0, R1, 997, 1000);
  newFork.applySwap(0, 1, D("5000000000000000000"));
  newFork.applySwap(0, 1, D("1000000000000000000"));
  newFork.applySwap(1, 0, D("4000000000000000000"));
  EXPECT_EQ(feed.curve()->balances()[0].toDec(), newFork.balances()[0].toDec());
}

// A finalised swap is irreversible: a reorg below the finalised height cannot drop it.
TEST(ReorgSafePoolFeedTest, FinalizedSwapsSurviveReorg)
{
  ConstantProductCurve seed(R0, R1, 997, 1000);
  AmmDexConnector conn = makeConn(seed);
  conn.setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});
  ReorgSafePoolFeed feed(conn, std::make_unique<ConstantProductCurve>(R0, R1, 997, 1000));

  feed.onSwap(1, true, D("5000000000000000000"));
  feed.finalize(1);  // block 1 irreversible
  EXPECT_EQ(feed.pendingCount(), 0u);
  feed.onSwap(2, true, D("2000000000000000000"));

  feed.reorg(1);  // a reorg that would roll back to height 1 (block 2 dropped)
  EXPECT_EQ(feed.pendingCount(), 0u);

  // Only the finalised swap remains applied.
  ConstantProductCurve onlyFinal(R0, R1, 997, 1000);
  onlyFinal.applySwap(0, 1, D("5000000000000000000"));
  EXPECT_EQ(feed.curve()->balances()[0].toDec(), onlyFinal.balances()[0].toDec());
}

}  // namespace
