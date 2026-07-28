/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/ledger.h"
#include "flox-venue/segregation.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr AssetId USD = 1;
constexpr uint64_t VENUE = 900;      // operational + fee capture
constexpr uint64_t INSURANCE = 901;  // insurance fund
Amount usd(double v) { return amountOf(Volume::fromDouble(v)); }

void test_segregation()
{
  std::printf("test_segregation\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(2500));
  led.deposit(3, USD, usd(500));
  led.deposit(VENUE, USD, usd(300));  // house money (not client)
  led.deposit(INSURANCE, USD, usd(10000));

  SegregationReport seg(led, {VENUE, INSURANCE});

  // Client money owed = sum of client accounts (house excluded).
  CHECK(seg.clientTotal(USD) == usd(4000));  // 1000 + 2500 + 500
  CHECK(seg.houseTotal(USD) == usd(10300));  // 300 + 10000

  // Custody fully backs client money.
  CHECK(seg.fullyBacked(USD, usd(4000)));
  CHECK(seg.fullyBacked(USD, usd(5000)));
  CHECK(seg.shortfall(USD, usd(4000)) == 0);

  // Custody deficit -> reportable breach.
  CHECK(!seg.fullyBacked(USD, usd(3999)));
  CHECK(seg.shortfall(USD, usd(3999)) == usd(1));
  CHECK(seg.shortfall(USD, usd(3000)) == usd(1000));

  // A reservation (buying power locked) still counts as client money owed --
  // reserved + available both belong to the client.
  led.reserve(1, USD, usd(400));
  CHECK(seg.clientTotal(USD) == usd(4000));  // unchanged: total = avail + reserved

  // A trade moving value between clients keeps the client total invariant; a fee
  // to the house reduces client money (and custody requirement) accordingly.
  led.debit(2, USD, usd(100));
  led.credit(1, USD, usd(90));
  led.credit(VENUE, USD, usd(10));           // 10 fee captured by the house
  CHECK(seg.clientTotal(USD) == usd(3990));  // 4000 - 10 fee
  CHECK(seg.houseTotal(USD) == usd(10310));
}

// A client with a DEBIT (negative) balance must not net down the segregation
// requirement: the firm still owes the credit clients their full balances, and a
// negative wallet is the firm's receivable, not a reduction of client money.
void test_segregation_no_debit_netting()
{
  std::printf("test_segregation_no_debit_netting\n");
  Ledger led;
  led.deposit(1, USD, usd(10000));  // client A is owed 10000
  led.credit(2, USD, -usd(3000));   // client B's wallet driven negative (e.g. funding)
  SegregationReport seg(led, {VENUE, INSURANCE});

  // Owed = A's 10000 only; B's -3000 is a receivable, NOT a -3000 to net out.
  CHECK(seg.clientTotal(USD) == usd(10000));  // netting would report 7000
  // Custody of 7000 must be flagged short by 3000, not "fully backed".
  CHECK(!seg.fullyBacked(USD, usd(7000)));
  CHECK(seg.shortfall(USD, usd(7000)) == usd(3000));
  CHECK(seg.fullyBacked(USD, usd(10000)));
}

}  // namespace

TEST(Segregation, EngineSuite)
{
  test_segregation();
  test_segregation_no_debit_netting();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
