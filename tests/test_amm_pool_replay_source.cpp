/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/amm_pool_replay_source.h"

#include "flox/backtest/constant_product_curve.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

// A constant-product pool snapshot with the given reserves (18-decimal tokens).
std::unique_ptr<INTokenCurve> cp(const char* baseRes, const char* quoteRes)
{
  return std::make_unique<ConstantProductCurve>(D(baseRes), D(quoteRes), 997, 1000);
}

// The driver replays recorded pool states through the connector: each snapshot
// re-points the curve and republishes the synthetic book, stamped at its time. As
// the reserves shift, the published price tracks them, reproducibly.
TEST(AmmPoolReplaySourceTest, ReplaysPriceOverSnapshots)
{
  ConstantProductCurve initial(D("1000000000000000000000"), D("2000000000000000000000"), 997, 1000);
  AmmDexConnector conn("amm", SymbolId{1}, initial, /*base*/ 0, /*quote*/ 1, /*baseDec*/ 18,
                       /*quoteDec*/ 18, /*levels*/ 3, /*levelSize*/ D("1000000000000000000"));

  std::vector<double> bestBid;
  std::vector<int64_t> stamps;
  conn.setCallbacks(
      [&](const BookUpdateEvent& ev)
      {
        ASSERT_FALSE(ev.update.bids.empty());
        bestBid.push_back(ev.update.bids.front().price.toDouble());
        stamps.push_back(static_cast<int64_t>(ev.update.exchangeTsNs));
      },
      [](const TradeEvent&) {});

  // Price (quote per base) starts at ~2.0, rises to ~2.2, falls to ~1.8.
  std::vector<AmmPoolSnapshot> snaps;
  snaps.push_back({100, cp("1000000000000000000000", "2000000000000000000000")});
  snaps.push_back({200, cp("1000000000000000000000", "2200000000000000000000")});
  snaps.push_back({300, cp("1000000000000000000000", "1800000000000000000000")});

  AmmPoolReplaySource src(conn, std::move(snaps));
  EXPECT_EQ(src.size(), 3u);
  ASSERT_TRUE(src.dataRange().has_value());
  EXPECT_EQ(src.dataRange()->start_ns, 100);
  EXPECT_EQ(src.dataRange()->end_ns, 300);

  src.run();
  EXPECT_TRUE(src.isFinished());

  ASSERT_EQ(bestBid.size(), 3u);
  EXPECT_EQ(stamps, (std::vector<int64_t>{100, 200, 300}));
  // The published price follows the recorded reserves.
  EXPECT_NEAR(bestBid[0], 2.0, 0.05);
  EXPECT_GT(bestBid[1], bestBid[0]);  // 2.2 > 2.0
  EXPECT_LT(bestBid[2], bestBid[0]);  // 1.8 < 2.0

  // Reproducible: replaying again yields the same prices.
  std::vector<double> first = bestBid;
  bestBid.clear();
  stamps.clear();
  src.reset();
  src.run();
  EXPECT_EQ(bestBid, first);
}

}  // namespace
