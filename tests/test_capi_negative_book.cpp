/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>

#include <cstdint>

// The C book accessors already have room for a negative best quote: they return
// a presence flag and write the price through an out parameter, so "no quote"
// and "a quote at a negative price" are two different answers at the ABI level.
// They do not currently give two different answers, because every one of them
// forwards to NLevelOrderBook::bestBid / bestAsk / mid / spread, which read a
// negative tick as an empty side.
TEST(CapiNegativeBook, BestQuotesCarryANegativePrice)
{
  FloxBookHandle book = flox_book_create(1.0);
  ASSERT_NE(book, nullptr);

  double p = 0;
  // Green control: an untouched book has no quote on either side.
  EXPECT_EQ(flox_book_best_bid(book, &p), 0);
  EXPECT_EQ(flox_book_best_ask(book, &p), 0);

  double bidsP[] = {-100.4};
  double bidsQ[] = {3.0};
  double asksP[] = {-99.6};
  double asksQ[] = {2.0};
  flox_book_apply_snapshot(book, bidsP, bidsQ, 1, asksP, asksQ, 1);

  // The levels are in the book: the depth accessors read them through the level
  // walk rather than the cached tick, and they report them.
  double lvlP[8] = {0}, lvlQ[8] = {0};
  ASSERT_EQ(flox_book_get_bids(book, lvlP, lvlQ, 8), 1u)
      << "the negative bid was not stored at all";
  ASSERT_LT(lvlP[0], 0.0);
  const double storedBid = lvlP[0];
  ASSERT_EQ(flox_book_get_asks(book, lvlP, lvlQ, 8), 1u)
      << "the negative ask was not stored at all";
  ASSERT_LT(lvlP[0], 0.0);
  const double storedAsk = lvlP[0];

  double bid = 0;
  ASSERT_EQ(flox_book_best_bid(book, &bid), 1)
      << "flox_book_best_bid signals no quote while the book holds one at " << storedBid;
  EXPECT_NEAR(bid, storedBid, 1e-9);

  double ask = 0;
  ASSERT_EQ(flox_book_best_ask(book, &ask), 1)
      << "flox_book_best_ask signals no quote while the book holds one at " << storedAsk;
  EXPECT_NEAR(ask, storedAsk, 1e-9);

  // Green control: clearing puts the side back to "no quote", and that answer
  // has to stay distinguishable from a negative price.
  flox_book_clear(book);
  EXPECT_EQ(flox_book_best_bid(book, &p), 0);
  EXPECT_EQ(flox_book_best_ask(book, &p), 0);

  flox_book_destroy(book);
}

TEST(CapiNegativeBook, MidSpreadAndCrossedAreReportedBelowZero)
{
  FloxBookHandle book = flox_book_create(1.0);
  ASSERT_NE(book, nullptr);

  double bidsP[] = {-101.0};
  double bidsQ[] = {1.0};
  double asksP[] = {-99.0};
  double asksQ[] = {1.0};
  flox_book_apply_snapshot(book, bidsP, bidsQ, 1, asksP, asksQ, 1);

  double mid = 0;
  ASSERT_EQ(flox_book_mid(book, &mid), 1) << "no mid on a book quoted below zero";
  EXPECT_NEAR(mid, -100.0, 1e-9);

  double spread = 0;
  ASSERT_EQ(flox_book_spread(book, &spread), 1) << "no spread on a book quoted below zero";
  EXPECT_NEAR(spread, 2.0, 1e-9);

  EXPECT_EQ(flox_book_is_crossed(book), 0);

  double crossBidP[] = {-99.0};
  double crossBidQ[] = {1.0};
  double crossAskP[] = {-101.0};
  double crossAskQ[] = {1.0};
  flox_book_apply_snapshot(book, crossBidP, crossBidQ, 1, crossAskP, crossAskQ, 1);
  EXPECT_EQ(flox_book_is_crossed(book), 1)
      << "a bid at -99.0 over an ask at -101.0 reads as a clean book";

  flox_book_destroy(book);
}

// needs: uint8_t flox_best_bid_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out);
// needs: uint8_t flox_best_ask_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out);
// needs: uint8_t flox_mid_price_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out);
//
// The strategy-side raw accessors spend the value 0 as their "no quote" answer
// (flox_capi.cpp: `return bid ? bid->raw() : 0;`), and the JS binding on top of
// them turns that into the number 0.0 rather than null. A raw of 0 is a price
// of exactly 0.0, which a book quoting through zero reaches, so the two answers
// collide inside the return type itself. That cannot be repaired without a
// second output channel, so the case is stated against the accessors it needs
// and is enabled once they exist.
TEST(CapiNegativeBook, StrategyRawBestQuotesSeparateNoQuoteFromAPriceOfZero)
{
#if defined(FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE)
  FloxStrategyHandle s = currentStrategyUnderTest();
  int64_t raw = 12345;
  EXPECT_EQ(flox_best_bid_raw_opt(s, 0, &raw), 0) << "an empty side must not report a quote";
  EXPECT_EQ(flox_best_bid_raw_opt(s, 1, &raw), 1) << "a bid at 0.0 must be reported";
  EXPECT_EQ(raw, 0);
#else
  FAIL() << "flox_best_bid_raw returns 0 both for 'no quote' and for a quote at a "
            "price of exactly 0.0; the ABI has no way to tell them apart. Needs "
            "flox_best_bid_raw_opt / flox_best_ask_raw_opt / flox_mid_price_raw_opt "
            "with a presence flag, and FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE to enable "
            "this case.";
#endif
}
