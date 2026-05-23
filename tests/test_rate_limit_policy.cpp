/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/rate_limit_policy.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr int64_t SEC = 1'000'000'000LL;
using Action = RateLimitPolicy::ActionKind;
}  // namespace

TEST(RateLimitPolicy, EmptyPolicyAllowsEverything)
{
  RateLimitPolicy p;
  for (int i = 0; i < 100; ++i)
  {
    EXPECT_TRUE(p.tryConsume(Action::Submit, i * SEC));
  }
}

TEST(RateLimitPolicy, BucketCapAllowsThenRejects)
{
  RateLimitPolicy p;
  p.addBucket("orders_10s", 10 * SEC, /*capacity=*/3);

  EXPECT_TRUE(p.tryConsume(Action::Submit, 0));
  EXPECT_TRUE(p.tryConsume(Action::Submit, 1 * SEC));
  EXPECT_TRUE(p.tryConsume(Action::Submit, 2 * SEC));
  // 4th in the same window is rejected.
  EXPECT_FALSE(p.tryConsume(Action::Submit, 3 * SEC));
  // Wait past the window — first slot frees up.
  EXPECT_TRUE(p.tryConsume(Action::Submit, 11 * SEC));
}

TEST(RateLimitPolicy, SlidingWindowEvictsExpiredEntries)
{
  RateLimitPolicy p;
  p.addBucket("orders_1s", 1 * SEC, 2);

  EXPECT_TRUE(p.tryConsume(Action::Submit, 0));
  EXPECT_TRUE(p.tryConsume(Action::Submit, 500'000'000LL));
  EXPECT_FALSE(p.tryConsume(Action::Submit, 800'000'000LL));
  // 1.1s in: first entry (t=0) just dropped out of the window.
  EXPECT_TRUE(p.tryConsume(Action::Submit, 1'100'000'000LL));
}

TEST(RateLimitPolicy, ReplaceUsesHeavierWeight)
{
  RateLimitPolicy p;
  // Capacity 4, replace weight 2 → only 2 replaces fit per window.
  p.addBucket("orders", 10 * SEC, /*capacity=*/4,
              /*submit=*/1, /*cancel=*/1, /*replace=*/2);

  EXPECT_TRUE(p.tryConsume(Action::Replace, 0));        // 2 used
  EXPECT_TRUE(p.tryConsume(Action::Replace, 1 * SEC));  // 4 used
  EXPECT_FALSE(p.tryConsume(Action::Submit, 2 * SEC));  // would overflow
}

TEST(RateLimitPolicy, BanTriggersAfterConsecutiveRejects)
{
  RateLimitPolicy p;
  p.addBucket("orders", 10 * SEC, /*capacity=*/1);
  p.setBan(/*after=*/3, /*duration=*/60 * SEC);

  EXPECT_TRUE(p.tryConsume(Action::Submit, 0));
  // Three rejects in a row → ban arms.
  EXPECT_FALSE(p.tryConsume(Action::Submit, 1 * SEC));
  EXPECT_FALSE(p.tryConsume(Action::Submit, 2 * SEC));
  EXPECT_FALSE(p.tryConsume(Action::Submit, 3 * SEC));
  // Even after the window clears, the ban window is still active.
  EXPECT_FALSE(p.tryConsume(Action::Submit, 20 * SEC));
  // After ban duration elapses, requests resume.
  EXPECT_TRUE(p.tryConsume(Action::Submit, 70 * SEC));
}

TEST(RateLimitPolicy, BanResetsConsecutiveCounter)
{
  RateLimitPolicy p;
  p.addBucket("orders", 10 * SEC, 1);
  p.setBan(2, 5 * SEC);

  EXPECT_TRUE(p.tryConsume(Action::Submit, 0));
  EXPECT_FALSE(p.tryConsume(Action::Submit, 1 * SEC));  // first reject
  EXPECT_FALSE(p.tryConsume(Action::Submit, 2 * SEC));  // arms ban
  // 7s in: ban expired, window has slot free.
  EXPECT_TRUE(p.tryConsume(Action::Submit, 11 * SEC));
  EXPECT_EQ(p.consecutiveRejects(), 0u);
}

TEST(RateLimitPolicy, MultipleBucketsAllMustAccept)
{
  RateLimitPolicy p;
  // Burst cap 20, minute cap 10: minute bucket is the bottleneck.
  p.addBucket("burst_1s", 1 * SEC, 20);
  p.addBucket("minute", 60 * SEC, 10);

  for (int i = 0; i < 10; ++i)
  {
    EXPECT_TRUE(p.tryConsume(Action::Submit, i * 100'000'000LL));
  }
  // 11th in the same minute fails on the minute bucket.
  EXPECT_FALSE(p.tryConsume(Action::Submit, 11 * 100'000'000LL));
}

TEST(RateLimitPolicy, BucketStatesReportUsage)
{
  RateLimitPolicy p;
  p.addBucket("burst", 10 * SEC, 5);
  p.tryConsume(Action::Submit, 0);
  p.tryConsume(Action::Submit, 1 * SEC);

  auto states = p.bucketStates(2 * SEC);
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0].name, "burst");
  EXPECT_EQ(states[0].used, 2u);
  EXPECT_EQ(states[0].capacity, 5u);
}

TEST(RateLimitPolicy, CannedProfileBinanceUmFutures)
{
  auto p = RateLimitPolicy::binance_um_futures();
  // 2 trading + 1 market_data + 1 account.
  EXPECT_EQ(p.bucketCount(), 4u);
  // Consume 50 orders rapidly — exactly the 10s cap.
  for (int i = 0; i < 50; ++i)
  {
    EXPECT_TRUE(p.tryConsume(Action::Submit, i * 1'000'000LL));
  }
  // 51st rejects.
  EXPECT_FALSE(p.tryConsume(Action::Submit, 51 * 1'000'000LL));
}

TEST(RateLimitPolicy, AtomicityAcrossBucketsOnReject)
{
  RateLimitPolicy p;
  p.addBucket("a", 10 * SEC, 5);
  p.addBucket("b", 10 * SEC, 1);

  EXPECT_TRUE(p.tryConsume(Action::Submit, 0));
  // Bucket A has room, B doesn't. Reject must not leave A partially
  // charged for the failed attempt.
  EXPECT_FALSE(p.tryConsume(Action::Submit, 1 * SEC));
  auto states = p.bucketStates(2 * SEC);
  ASSERT_EQ(states.size(), 2u);
  EXPECT_EQ(states[0].used, 1u);  // A: only the first accepted call
  EXPECT_EQ(states[1].used, 1u);  // B: full
}

// === T049: per-endpoint-family pools ===

using Family = RateLimitPolicy::EndpointFamily;

TEST(RateLimitPolicy, TradingPoolDoesNotChargeMarketDataBucket)
{
  // Exhaust the trading pool; market_data is independent.
  RateLimitPolicy p;
  p.addBucket("trading", 10 * SEC, 1);
  p.addFamilyBucket(Family::MarketData, "md", 10 * SEC, 1);

  EXPECT_TRUE(p.tryConsume(Action::Submit, 0));
  // Trading pool full — submit rejected.
  EXPECT_FALSE(p.tryConsume(Action::Submit, 1 * SEC));
  // Market-data query is independent — still goes through.
  EXPECT_TRUE(p.tryConsume(Action::QueryMarketData, 2 * SEC));
  // Account family has no bucket → still allowed (no enforcement
  // configured for that family).
  EXPECT_TRUE(p.tryConsume(Action::QueryAccount, 3 * SEC));
}

TEST(RateLimitPolicy, MarketDataPoolDoesNotChargeTradingBucket)
{
  // Exhaust market-data pool, then submit a trade.
  RateLimitPolicy p;
  p.addBucket("trading", 10 * SEC, 5);
  p.addFamilyBucket(Family::MarketData, "md", 10 * SEC, 1);

  EXPECT_TRUE(p.tryConsume(Action::QueryMarketData, 0));
  EXPECT_FALSE(p.tryConsume(Action::QueryMarketData, 1 * SEC));
  // Trading pool untouched.
  EXPECT_TRUE(p.tryConsume(Action::Submit, 2 * SEC));
  EXPECT_TRUE(p.tryConsume(Action::Submit, 3 * SEC));
}

TEST(RateLimitPolicy, AccountPoolIndependent)
{
  RateLimitPolicy p;
  p.addBucket("trading", 10 * SEC, 5);
  p.addFamilyBucket(Family::Account, "account", 10 * SEC, 2);

  EXPECT_TRUE(p.tryConsume(Action::QueryAccount, 0));
  EXPECT_TRUE(p.tryConsume(Action::QueryAccount, 1 * SEC));
  // 3rd account query: rejected.
  EXPECT_FALSE(p.tryConsume(Action::QueryAccount, 2 * SEC));
  // Trading still good.
  EXPECT_TRUE(p.tryConsume(Action::Submit, 3 * SEC));
}

TEST(RateLimitPolicy, BinanceProfilePopulatesAllThreeFamilies)
{
  auto p = RateLimitPolicy::binance_um_futures();
  auto states = p.bucketStates(0);
  size_t trading = 0, md = 0, acct = 0;
  for (const auto& s : states)
  {
    switch (s.endpointFamily)
    {
      case Family::Trading:
        ++trading;
        break;
      case Family::MarketData:
        ++md;
        break;
      case Family::Account:
        ++acct;
        break;
    }
  }
  EXPECT_GE(trading, 2u);
  EXPECT_GE(md, 1u);
  EXPECT_GE(acct, 1u);
}

TEST(RateLimitPolicy, FamilyOfActionIsCorrect)
{
  using K = RateLimitPolicy::ActionKind;
  EXPECT_EQ(RateLimitPolicy::familyOf(K::Submit), Family::Trading);
  EXPECT_EQ(RateLimitPolicy::familyOf(K::Cancel), Family::Trading);
  EXPECT_EQ(RateLimitPolicy::familyOf(K::Replace), Family::Trading);
  EXPECT_EQ(RateLimitPolicy::familyOf(K::QueryAccount), Family::Account);
  EXPECT_EQ(RateLimitPolicy::familyOf(K::QueryMarketData), Family::MarketData);
}
