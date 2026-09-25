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

// The bid case above is only a third of the contract. The ask and the mid have
// the same two answers to keep apart -- a quote at a price of exactly 0.0 and
// no quote at all -- and they read different state to produce them, so each one
// is asserted the same way against the same set of books: present with the raw
// price written out, absent with the flag clear.
TEST(CapiNegativeBook, OptionalRawBestQuotesAnswerEveryBookState)
{
#if defined(FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE)
  const auto& books = bookStatesUnderTest();
  FloxStrategyHandle s = books.strategy;
  int64_t raw = 0;

  // A book holding a bid and no ask: the bid is a real quote at 0.0, and the
  // two accessors that need an ask have nothing to answer with.
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.bidAtZero, &raw), 1)
      << "a bid at exactly 0.0 is reported as no bid";
  EXPECT_EQ(raw, 0);
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.bidAtZero, &raw), 0)
      << "an empty ask side reports a quote";
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.bidAtZero, &raw), 0)
      << "a one-sided book has a mid";

  // The mirror image: an ask and no bid.
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.askOnly, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(5.0));
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.askOnly, &raw), 0)
      << "an empty bid side reports a quote";
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.askOnly, &raw), 0);

  // An ask at exactly 0.0 -- the value the plain int64_t accessors spend as
  // their "no quote" answer.
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.askAtZero, &raw), 1)
      << "an ask at exactly 0.0 is reported as no ask";
  EXPECT_EQ(raw, 0);
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.askAtZero, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(-2.0));
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.askAtZero, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(-1.0));

  // A mid of exactly 0.0, from a two-sided book quoting either side of zero.
  // The mid has to be the mid and not one of the two quotes it is built from.
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.midAtZero, &raw), 1)
      << "a mid of exactly 0.0 is reported as no mid";
  EXPECT_EQ(raw, 0);
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.midAtZero, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(-1.0));
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.midAtZero, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(1.0));

  // A book quoted entirely below zero: three distinct prices, so a mid that
  // answers with the bid, or an ask that answers with the bid, is visible.
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.belowZero, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(-101.0));
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.belowZero, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(-99.0));
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.belowZero, &raw), 1);
  EXPECT_EQ(raw, flox_price_from_double(-100.0));

  // A symbol with no book at all: all three absent.
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.noSymbol, &raw), 0);
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.noSymbol, &raw), 0);
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.noSymbol, &raw), 0);
#else
  FAIL() << "needs flox_best_bid_raw_opt / flox_best_ask_raw_opt / "
            "flox_mid_price_raw_opt and FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE";
#endif
}

// "0 with price_out untouched", in the header's own words. A caller that seeds
// its local and then reads it back on the strength of the flag must find what
// it put there, so a flag of 0 arriving next to a write of 0 -- indistinguishable
// from a quote at 0.0 for anyone who forgets to check the flag -- is caught here.
// Each call gets its own local, since a local shared between two calls only ever
// shows the last write.
TEST(CapiNegativeBook, OptionalRawBestQuotesLeavePriceOutUntouchedWhenThereIsNoQuote)
{
#if defined(FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE)
  const auto& books = bookStatesUnderTest();
  FloxStrategyHandle s = books.strategy;
  constexpr int64_t kSentinel = -987654321;

  int64_t bidOnEmptySide = kSentinel;
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.askOnly, &bidOnEmptySide), 0);
  EXPECT_EQ(bidOnEmptySide, kSentinel) << "flox_best_bid_raw_opt wrote a price it has none of";

  int64_t bidOnAbsentBook = kSentinel;
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.noSymbol, &bidOnAbsentBook), 0);
  EXPECT_EQ(bidOnAbsentBook, kSentinel);

  int64_t askOnEmptySide = kSentinel;
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.bidAtZero, &askOnEmptySide), 0);
  EXPECT_EQ(askOnEmptySide, kSentinel) << "flox_best_ask_raw_opt wrote a price it has none of";

  int64_t askOnAbsentBook = kSentinel;
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.noSymbol, &askOnAbsentBook), 0);
  EXPECT_EQ(askOnAbsentBook, kSentinel);

  int64_t midOnOneSidedBook = kSentinel;
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.bidAtZero, &midOnOneSidedBook), 0);
  EXPECT_EQ(midOnOneSidedBook, kSentinel)
      << "flox_mid_price_raw_opt wrote a price it has none of";

  int64_t midOnAbsentBook = kSentinel;
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.noSymbol, &midOnAbsentBook), 0);
  EXPECT_EQ(midOnAbsentBook, kSentinel);

  // Green control: the same locals are written when there is a quote, so the
  // assertions above are pinning "untouched on absence" and not "never writes".
  int64_t present = kSentinel;
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.bidAtZero, &present), 1);
  EXPECT_EQ(present, 0);
#else
  FAIL() << "needs FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE";
#endif
}

// "price_out may be NULL when only the flag is wanted" -- a caller asking
// whether a side is quoted at all, which is exactly what the JS bindings do on
// the way to returning null. Every caller in the tree passes a real pointer, so
// a dereference that stopped checking would never be noticed here.
TEST(CapiNegativeBook, OptionalRawBestQuotesAcceptANullPriceOut)
{
#if defined(FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE)
  const auto& books = bookStatesUnderTest();
  FloxStrategyHandle s = books.strategy;

  // With a quote on the other end: the flag still comes back, nothing is
  // written anywhere.
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.bidAtZero, nullptr), 1);
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.askAtZero, nullptr), 1);
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.midAtZero, nullptr), 1);
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.belowZero, nullptr), 1);
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.belowZero, nullptr), 1);
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.belowZero, nullptr), 1);

  // And with nothing to report.
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.askOnly, nullptr), 0);
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.bidAtZero, nullptr), 0);
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.bidAtZero, nullptr), 0);
  EXPECT_EQ(flox_best_bid_raw_opt(s, books.noSymbol, nullptr), 0);
  EXPECT_EQ(flox_best_ask_raw_opt(s, books.noSymbol, nullptr), 0);
  EXPECT_EQ(flox_mid_price_raw_opt(s, books.noSymbol, nullptr), 0);
#else
  FAIL() << "needs FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE";
#endif
}
