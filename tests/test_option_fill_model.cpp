#include <gtest/gtest.h>

#include "flox/backtest/option_fill_model.h"

#include <vector>

using namespace flox;

// A single-leg buy fills above the mid but no worse than the ask — it crosses
// part of the spread, not the whole of it.
TEST(OptionFillModelTest, SingleLegBuyCrossesPartOfSpread)
{
  const OptionBBO q{1.00, 1.40};  // mid 1.20, half-spread 0.20
  const auto r = fillSingleLeg(Side::BUY, q);
  ASSERT_TRUE(r.filled);
  EXPECT_GT(r.price, q.mid());
  EXPECT_LT(r.price, q.ask);
  EXPECT_DOUBLE_EQ(r.slippage, 0.75 * q.halfSpread());  // ORATS single-leg
  EXPECT_DOUBLE_EQ(r.price, q.mid() + 0.75 * q.halfSpread());
}

// A sell is the mirror: below the mid, above the bid.
TEST(OptionFillModelTest, SingleLegSellMirrors)
{
  const OptionBBO q{1.00, 1.40};
  const auto r = fillSingleLeg(Side::SELL, q);
  ASSERT_TRUE(r.filled);
  EXPECT_LT(r.price, q.mid());
  EXPECT_GT(r.price, q.bid);
  EXPECT_DOUBLE_EQ(r.price, q.mid() - 0.75 * q.halfSpread());
}

// An absent or locked/crossed quote never fakes a fill.
TEST(OptionFillModelTest, NoQuoteDoesNotFakeFill)
{
  EXPECT_FALSE(fillSingleLeg(Side::BUY, OptionBBO{0.0, 0.0}).filled);    // no quote
  EXPECT_FALSE(fillSingleLeg(Side::BUY, OptionBBO{1.40, 1.40}).filled);  // locked
  EXPECT_FALSE(fillSingleLeg(Side::BUY, OptionBBO{1.50, 1.40}).filled);  // crossed
  EXPECT_FALSE(fillSingleLeg(Side::SELL, OptionBBO{0.0, 1.40}).filled);  // one-sided
}

// Slippage per leg shrinks as legs are added: a four-leg package crosses less of
// each spread than four separate single-leg orders would.
TEST(OptionFillModelTest, MultiLegSlippageIsNonLinear)
{
  EXPECT_DOUBLE_EQ(legCrossFraction(1), 0.75);
  EXPECT_DOUBLE_EQ(legCrossFraction(4), 0.56);
  EXPECT_GT(legCrossFraction(1), legCrossFraction(2));
  EXPECT_GT(legCrossFraction(2), legCrossFraction(3));
  EXPECT_GT(legCrossFraction(3), legCrossFraction(4));
  EXPECT_DOUBLE_EQ(legCrossFraction(10), 0.56);  // flat past four
}

// A multi-leg spread fills atomically and prices off the package cross fraction.
TEST(OptionFillModelTest, SpreadFillsAtomicallyWithPackageImprovement)
{
  // Buy a vertical: long the 1.00/1.40 call, short the 0.40/0.60 call.
  std::vector<SpreadLeg> legs = {
      {Side::BUY, 1.0, OptionBBO{1.00, 1.40}},
      {Side::SELL, 1.0, OptionBBO{0.40, 0.60}},
  };
  const auto r = fillSpread(legs);
  ASSERT_TRUE(r.filled);

  const double cross = legCrossFraction(2);
  const double netMid = 1.20 - 0.50;          // long mid - short mid
  const double slip = cross * (0.20 + 0.10);  // sum qty * cross * half
  EXPECT_DOUBLE_EQ(r.slippage, slip);
  EXPECT_DOUBLE_EQ(r.netDebit, netMid + slip);
  EXPECT_GT(r.slippage, 0.0);  // crossing always costs
}

// One missing leg quote fails the whole atomic package.
TEST(OptionFillModelTest, SpreadFailsAtomicallyOnMissingLeg)
{
  std::vector<SpreadLeg> legs = {
      {Side::BUY, 1.0, OptionBBO{1.00, 1.40}},
      {Side::SELL, 1.0, OptionBBO{0.0, 0.0}},  // no quote on this leg
  };
  EXPECT_FALSE(fillSpread(legs).filled);
}

// The headline A/B: a buy-then-sell round trip is flat at the mid but loses the
// crossed spread under a realistic fill — i.e. mid/close fills overstate PnL.
TEST(OptionFillModelTest, RealisticFillCostsMoreThanMid)
{
  const OptionBBO q{1.00, 1.40};
  const auto buy = fillSingleLeg(Side::BUY, q);
  const auto sell = fillSingleLeg(Side::SELL, q);
  ASSERT_TRUE(buy.filled);
  ASSERT_TRUE(sell.filled);

  const double midRoundTrip = 0.0;                      // buy and sell at mid -> flat
  const double realRoundTrip = sell.price - buy.price;  // negative: you lose the spread
  EXPECT_LT(realRoundTrip, midRoundTrip);
  EXPECT_DOUBLE_EQ(realRoundTrip, -2.0 * 0.75 * q.halfSpread());
}

// Widening models close-proximity spreads: the mid is unchanged, the spread
// grows, and a fill therefore costs more than on the un-widened quote.
TEST(OptionFillModelTest, WidenQuoteIncreasesFillCost)
{
  const OptionBBO q{1.00, 1.40};
  const OptionBBO wide = widenQuote(q, 2.0);
  EXPECT_DOUBLE_EQ(wide.mid(), q.mid());
  EXPECT_DOUBLE_EQ(wide.width(), 2.0 * q.width());

  const auto tight = fillSingleLeg(Side::BUY, q);
  const auto loose = fillSingleLeg(Side::BUY, wide);
  EXPECT_GT(loose.price, tight.price);
}
