/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-venue/ledger.h"

#include <gtest/gtest.h>

using namespace flox;
using namespace flox::venue;

namespace
{
constexpr AssetId USD = 1;
constexpr AssetId BTC = 0;
Amount amt(double v) { return amountOf(Volume::fromDouble(v)); }
// __int128 is not gtest-printable; compare the low 64 bits (test values fit).
int64_t i64(Amount a) { return static_cast<int64_t>(a); }
}  // namespace

TEST(Ledger, ReserveReleaseConservesTotal)
{
  Ledger l;
  l.deposit(1, USD, amt(1000));
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(1000)));

  EXPECT_TRUE(l.reserve(1, USD, amt(400)));
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(600)));
  EXPECT_EQ(i64(l.reserved(1, USD)), i64(amt(400)));
  EXPECT_EQ(i64(l.total(1, USD)), i64(amt(1000)));  // avail+rsvd conserved

  EXPECT_FALSE(l.reserve(1, USD, amt(700)));  // insufficient available -> no change
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(600)));

  l.release(1, USD, amt(400));
  EXPECT_EQ(i64(l.reserved(1, USD)), 0);
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(1000)));
}

TEST(Ledger, FillSettlementConservesAcrossAccounts)
{
  Ledger l;
  l.deposit(2, USD, amt(300));  // buyer: 300 USD
  l.deposit(1, BTC, amt(5));    // seller: 5 BTC

  // Pre-trade reservations for a 3 BTC @ 100 fill.
  ASSERT_TRUE(l.reserve(2, USD, amt(300)));
  ASSERT_TRUE(l.reserve(1, BTC, amt(3)));
  const Amount initUsd = l.total(2, USD) + l.total(1, USD);
  const Amount initBtc = l.total(2, BTC) + l.total(1, BTC);

  // Settle: buyer pays 300 USD from reserved, seller delivers 3 BTC from reserved.
  l.spendReserved(2, USD, amt(300));
  l.credit(1, USD, amt(300));
  l.spendReserved(1, BTC, amt(3));
  l.credit(2, BTC, amt(3));

  // Conservation across both accounts, per asset.
  EXPECT_EQ(i64(l.total(2, USD) + l.total(1, USD)), i64(initUsd));
  EXPECT_EQ(i64(l.total(2, BTC) + l.total(1, BTC)), i64(initBtc));
  // Buyer received base, seller received quote.
  EXPECT_EQ(i64(l.available(2, BTC)), i64(amt(3)));
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(300)));
  EXPECT_EQ(i64(l.reserved(2, USD)), 0);
  EXPECT_EQ(i64(l.reserved(1, BTC)), 0);             // all 3 reserved BTC delivered
  EXPECT_EQ(i64(l.available(1, BTC)), i64(amt(2)));  // 2 never-reserved BTC untouched
}

TEST(Ledger, DebitIsAllOrNothing)
{
  Ledger l;
  l.deposit(1, USD, amt(100));
  EXPECT_FALSE(l.debit(1, USD, amt(150)));  // insufficient -> no-op
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(100)));
  EXPECT_TRUE(l.debit(1, USD, amt(40)));
  EXPECT_EQ(i64(l.available(1, USD)), i64(amt(60)));
}

TEST(Ledger, ForEachBalanceEnumeratesTotals)
{
  Ledger l;
  l.deposit(7, USD, amt(500));
  l.reserve(7, USD, amt(200));  // 300 avail + 200 rsvd = 500 total
  Amount seen = 0;
  bool found = false;
  l.forEachBalance(
      [&](uint64_t acct, AssetId asset, Amount tot)
      {
        if (acct == 7 && asset == USD)
        {
          found = true;
          seen = tot;
        }
      });
  EXPECT_TRUE(found);
  EXPECT_EQ(i64(seen), i64(amt(500)));  // total = avail + reserved
}
