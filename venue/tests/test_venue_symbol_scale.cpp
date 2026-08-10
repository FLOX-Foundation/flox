/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-venue/cross_margin.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"

#include <gtest/gtest.h>

using namespace flox;
using namespace flox::venue;

namespace
{
constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 900;

int64_t i64(Amount a) { return static_cast<int64_t>(a); }

// A meme-coin-shaped symbol: whole-token quantities (qtyScale 1, so supplies of
// 1e12+ tokens fit int64), prices at the default 1e8 scale.
SymbolConfig memeCfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  c.priceScale = Price::Scale;
  c.qtyScale = 1;
  return c;
}

NewOrder rawOrder(OrderId id, Side s, int64_t praw, int64_t qraw, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = Price::fromRaw(praw);
  o.quantity = Quantity::fromRaw(qraw);
  o.accountId = acct;
  return o;
}
}  // namespace

TEST(SymbolScale, NotionalRawMatchesLegacyAtDefaultScale)
{
  // Bit-identical to the fixed-scale expression it replaced.
  for (int64_t praw : {int64_t{1}, int64_t{9999}, int64_t{12'345'678'901}})
  {
    for (int64_t qraw : {int64_t{1}, int64_t{307}, int64_t{5'000'000'000}})
    {
      const Amount legacy = static_cast<Amount>(praw) * qraw / Price::Scale;
      EXPECT_EQ(i64(notionalRaw(praw, qraw, Price::Scale, Quantity::Scale)), i64(legacy));
      EXPECT_EQ(i64(notionalRaw(-praw, qraw, Price::Scale, Quantity::Scale)), i64(-legacy));
    }
  }
}

TEST(SymbolScale, NotionalRawCoarseAndFineQtyScales)
{
  // qtyScale 1: 1e12 tokens at 0.00002 quote = 2e7 quote = raw 2e15.
  EXPECT_EQ(i64(notionalRaw(2000, 1'000'000'000'000, Price::Scale, 1)), 2'000'000'000'000'000);
  // Multiply-before-divide: 55 tokens at 0.00002 = 0.0011 quote = raw 110000.
  // Divide-first would truncate to 0.
  EXPECT_EQ(i64(notionalRaw(2000, 55, Price::Scale, 1)), 110'000);
  // qtyScale finer than money (1e10): 3 tokens (qraw 3e10) at 100.0 = 300 quote.
  EXPECT_EQ(i64(notionalRaw(10'000'000'000, 30'000'000'000, Price::Scale, 10'000'000'000)),
            30'000'000'000);
  // Truncation is toward zero for negative deltas, like the legacy expression.
  EXPECT_EQ(i64(notionalRaw(-3, 7, Price::Scale, Quantity::Scale)), 0);

  // Magnitudes past the multiply-first guard fall back to divide-first instead
  // of overflowing (no UB on adversarial input).
  const int64_t big = 9'000'000'000'000'000'000;
  const Amount prod = static_cast<Amount>(big) * big;
  EXPECT_TRUE(notionalRaw(big, big, Price::Scale, 1) == prod / Price::Scale * kMoneyScale);
}

TEST(SymbolScale, ScalesValid)
{
  EXPECT_TRUE(scalesValid(Price::Scale, Quantity::Scale));
  EXPECT_TRUE(scalesValid(Price::Scale, 1));
  EXPECT_TRUE(scalesValid(1, 10'000'000'000));
  EXPECT_FALSE(scalesValid(0, 1));
  EXPECT_FALSE(scalesValid(Price::Scale, 0));
  EXPECT_FALSE(scalesValid(Price::Scale, 3));            // does not divide 1e8
  EXPECT_TRUE(scalesValid(Price::Scale, 300'000'000));   // multiple of 1e8: ok
  EXPECT_FALSE(scalesValid(Price::Scale, 150'000'000));  // neither divides
}

TEST(SymbolScale, SpotSettlementAtCoarseQtyScale)
{
  Ledger led;
  const int64_t qraw = 1'000'000'000'000;  // 1e12 tokens
  const int64_t praw = 2000;               // 0.00002 quote
  const Amount notional = 2'000'000'000'000'000;

  led.deposit(1, BASE, qraw);       // seller: base balances are qty raw
  led.deposit(2, QUOTE, notional);  // buyer: exactly the notional

  MatchingEngine<MatchingBook> eng(memeCfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);

  eng.submit(InboundCommand{rawOrder(1, Side::SELL, praw, qraw, 1)}, 0);
  eng.submit(InboundCommand{rawOrder(2, Side::BUY, praw, qraw, 2)}, 1);

  EXPECT_EQ(i64(led.available(2, BASE)), qraw);
  EXPECT_EQ(i64(led.available(1, QUOTE)), i64(notional));
  EXPECT_EQ(i64(led.available(2, QUOTE)), 0);
  EXPECT_EQ(i64(led.total(1, BASE) + led.total(2, BASE)), qraw);
  EXPECT_EQ(i64(led.total(1, QUOTE) + led.total(2, QUOTE)), i64(notional));
}

TEST(SymbolScale, TinyTradeNotTruncatedToZero)
{
  Ledger led;
  led.deposit(1, BASE, 55);
  led.deposit(2, QUOTE, 110'000);

  MatchingEngine<MatchingBook> eng(memeCfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);

  eng.submit(InboundCommand{rawOrder(1, Side::SELL, 2000, 55, 1)}, 0);
  eng.submit(InboundCommand{rawOrder(2, Side::BUY, 2000, 55, 2)}, 1);

  EXPECT_EQ(i64(led.available(1, QUOTE)), 110'000);  // seller paid in full
  EXPECT_EQ(i64(led.available(2, BASE)), 55);
}

TEST(SymbolScale, EquivalentEconomicsAcrossScales)
{
  // The same trade (3 tokens at 100.0 quote) settles the same quote money
  // whether the symbol runs at the default scale or at qtyScale 1e6.
  auto run = [](int64_t qtyScale, int64_t qraw) -> Amount
  {
    Ledger led;
    led.deposit(1, BASE, qraw);
    led.deposit(2, QUOTE, 40'000'000'000);

    SymbolConfig c;
    c.id = SYM;
    c.baseAsset = BASE;
    c.quoteAsset = QUOTE;
    c.qtyScale = qtyScale;
    MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
    eng.setLedger(&led, VENUE_ACCT);

    eng.submit(InboundCommand{rawOrder(1, Side::SELL, 10'000'000'000, qraw, 1)}, 0);
    eng.submit(InboundCommand{rawOrder(2, Side::BUY, 10'000'000'000, qraw, 2)}, 1);
    return led.available(1, QUOTE);
  };

  const Amount atDefault = run(Quantity::Scale, 300'000'000);  // 3.0 at 1e8
  const Amount atMillion = run(1'000'000, 3'000'000);          // 3.0 at 1e6
  EXPECT_EQ(i64(atDefault), 30'000'000'000);                   // 300 quote raw
  EXPECT_EQ(i64(atMillion), i64(atDefault));
}

TEST(SymbolScale, PerpMarginAndPnlAtCoarseQtyScale)
{
  Ledger led;
  led.deposit(1, QUOTE, 2'000'000'000'000'000);  // 2e7 quote
  led.deposit(2, QUOTE, 2'000'000'000'000'000);

  SymbolConfig c = memeCfg();
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.maintenanceMarginBps = 500;
  MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);

  // 1e6 tokens at 100.0: notional 1e8 quote raw*1e8 = 1e16; IM 10% = 1e15.
  const int64_t praw = 10'000'000'000;
  const int64_t qraw = 1'000'000;
  eng.submit(InboundCommand{rawOrder(1, Side::BUY, praw, qraw, 1)}, 0);
  EXPECT_EQ(i64(led.reserved(1, QUOTE)), 1'000'000'000'000'000);

  eng.submit(InboundCommand{rawOrder(2, Side::SELL, praw, qraw, 2)}, 1);

  // Mark +1.0 quote on 1e6 tokens = 1e6 quote = raw 1e14.
  EXPECT_EQ(i64(eng.unrealizedPnlRaw(1, Price::fromRaw(10'100'000'000))), 100'000'000'000'000);
  EXPECT_EQ(i64(eng.unrealizedPnlRaw(2, Price::fromRaw(10'100'000'000))), -100'000'000'000'000);
}

TEST(SymbolScale, CrossMarginAtCoarseQtyScale)
{
  Ledger led;
  led.deposit(1, QUOTE, 1'000'000'000'000'000);
  CrossMarginManager cm(led, QUOTE, VENUE_ACCT);
  cm.configureSymbol(SYM, 1000, 500, Price::Scale, 1);

  cm.applyFill(1, SYM, Side::BUY, 1'000'000, 10'000'000'000);
  cm.setMark(SYM, Price::fromRaw(10'100'000'000));

  // Equity = wallet 1e15 + uPnL (mark +1.0 on 1e6 tokens = raw 1e14).
  EXPECT_EQ(i64(cm.equity(1)), 1'100'000'000'000'000);
  // IM at mark: 1e6 tokens * 101.0 * 10% = 1.01e7 quote = raw 1.01e15.
  EXPECT_EQ(i64(cm.initialMargin(1)), 1'010'000'000'000'000);
  EXPECT_EQ(i64(cm.openInterestRaw()), 10'100'000'000'000'000);
}

TEST(SymbolScale, MetricsVolumeNormalizedToMoney)
{
  Metrics m;
  m.setSymbolScales(SYM, Price::Scale, 1);

  Trade t;
  t.symbol = SYM;
  t.price = Price::fromRaw(2000);
  t.quantity = Quantity::fromRaw(1'000'000'000'000);
  m.observe(OutboundEvent{t});

  EXPECT_EQ(i64(static_cast<Amount>(m.volumeRaw)), 2'000'000'000'000'000);
}
