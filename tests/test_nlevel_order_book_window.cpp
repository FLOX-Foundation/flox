/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/common.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <vector>

using namespace flox;

namespace
{

// Standalone event builder: these cases need their own tick size and level
// count, so they cannot share the pooled fixture in test_nlevel_order_book.cpp.
class Update
{
 public:
  explicit Update(BookUpdateType type) : _res(_buf, sizeof(_buf)), _ev(&_res)
  {
    _ev.update.type = type;
  }

  Update& bid(int64_t priceRaw, double qty)
  {
    _ev.update.bids.push_back({Price::fromRaw(priceRaw), Quantity::fromDouble(qty)});
    return *this;
  }

  Update& ask(int64_t priceRaw, double qty)
  {
    _ev.update.asks.push_back({Price::fromRaw(priceRaw), Quantity::fromDouble(qty)});
    return *this;
  }

  const BookUpdateEvent& event() const { return _ev; }

 private:
  std::byte _buf[65536];
  std::pmr::monotonic_buffer_resource _res;
  BookUpdateEvent _ev;
};

constexpr int64_t kTenthRaw = 10000000;  // 0.1 at a price scale of 1e8

}  // namespace

// A delta-only feed walks the market off the end of the tick window. Bybit and
// Bitget re-send a snapshot only on connect and on a sequence gap, so nothing
// re-anchors the window in between: the book used to drop every level past the
// edge and go dark until the next reconnect.
TEST(NLevelOrderBookWindow, SurvivesDeltaOnlyDrift)
{
  constexpr size_t kLevels = 512;
  constexpr int64_t kDepth = 25;
  constexpr int64_t kBidTop = 599999;  // 59999.90 at a tick of 0.1
  constexpr int64_t kAskTop = 600001;  // 60000.10

  NLevelOrderBook<kLevels> book{Price::fromRaw(kTenthRaw)};

  {
    Update snap(BookUpdateType::SNAPSHOT);
    for (int64_t k = 0; k < kDepth; ++k)
    {
      snap.bid((kBidTop - k) * kTenthRaw, 1.0);
      snap.ask((kAskTop + k) * kTenthRaw, 1.0);
    }
    book.applyBookUpdate(snap.event());
  }

  ASSERT_TRUE(book.bestBid().has_value());
  ASSERT_EQ(book.bestBid()->raw(), kBidTop * kTenthRaw);

  // 600 ticks of upward drift is $60 on a 0.1-tick instrument. Each step lifts
  // both bands by one tick: add the new top level, drop the stale bottom one.
  constexpr int64_t kSteps = 600;
  for (int64_t s = 1; s <= kSteps; ++s)
  {
    Update d(BookUpdateType::DELTA);
    d.bid((kBidTop + s) * kTenthRaw, 1.0);
    d.bid((kBidTop + s - kDepth) * kTenthRaw, 0.0);
    d.ask((kAskTop + s + kDepth - 1) * kTenthRaw, 1.0);
    d.ask((kAskTop + s - 1) * kTenthRaw, 0.0);
    book.applyBookUpdate(d.event());

    ASSERT_TRUE(book.bestBid().has_value()) << "bid side went dark at +" << s << " ticks";
    ASSERT_TRUE(book.bestAsk().has_value()) << "ask side went dark at +" << s << " ticks";
    ASSERT_EQ(book.bestBid()->raw(), (kBidTop + s) * kTenthRaw) << "at +" << s << " ticks";
    ASSERT_EQ(book.bestAsk()->raw(), (kAskTop + s) * kTenthRaw) << "at +" << s << " ticks";
  }

  // Depth behind the touch has to survive the re-anchor, not just the top level.
  EXPECT_EQ(book.getBidLevels(kDepth).size(), static_cast<size_t>(kDepth));
  EXPECT_EQ(book.getAskLevels(kDepth).size(), static_cast<size_t>(kDepth));
  EXPECT_EQ(book.bidAtPrice(Price::fromRaw((kBidTop + kSteps - 5) * kTenthRaw)).toDouble(), 1.0);
}

TEST(NLevelOrderBookWindow, SurvivesDownwardDeltaOnlyDrift)
{
  constexpr size_t kLevels = 512;
  constexpr int64_t kBidTop = 300000;
  constexpr int64_t kAskTop = 300002;

  NLevelOrderBook<kLevels> book{Price::fromRaw(kTenthRaw)};

  {
    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(kBidTop * kTenthRaw, 1.0).ask(kAskTop * kTenthRaw, 1.0);
    book.applyBookUpdate(snap.event());
  }

  for (int64_t s = 1; s <= 900; ++s)
  {
    Update d(BookUpdateType::DELTA);
    d.bid((kBidTop - s) * kTenthRaw, 1.0);
    d.bid((kBidTop - s + 1) * kTenthRaw, 0.0);
    d.ask((kAskTop - s) * kTenthRaw, 1.0);
    d.ask((kAskTop - s + 1) * kTenthRaw, 0.0);
    book.applyBookUpdate(d.event());

    ASSERT_EQ(book.bestBid().value_or(Price{}).raw(), (kBidTop - s) * kTenthRaw)
        << "at -" << s << " ticks";
    ASSERT_EQ(book.bestAsk().value_or(Price{}).raw(), (kAskTop - s) * kTenthRaw)
        << "at -" << s << " ticks";
  }
}

// A level far enough away that keeping it would cost the whole current book is
// still dropped. Re-anchoring must never trade live depth for one outlier.
TEST(NLevelOrderBookWindow, IgnoresLevelsTooFarToCoexistWithTheBook)
{
  constexpr size_t kLevels = 512;
  NLevelOrderBook<kLevels> book{Price::fromRaw(kTenthRaw)};

  {
    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(600000 * kTenthRaw, 1.0).ask(600002 * kTenthRaw, 1.0);
    book.applyBookUpdate(snap.event());
  }

  Update d(BookUpdateType::DELTA);
  d.bid(1 * kTenthRaw, 5.0);  // 0.1 while the market is at 60000
  book.applyBookUpdate(d.event());

  EXPECT_EQ(book.bestBid().value_or(Price{}).raw(), 600000 * kTenthRaw);
  EXPECT_EQ(book.bestAsk().value_or(Price{}).raw(), 600002 * kTenthRaw);
  EXPECT_EQ(book.bidAtPrice(Price::fromRaw(1 * kTenthRaw)).toDouble(), 0.0);
}

// A removal names a price the book does not hold, so it can never justify
// moving the window.
TEST(NLevelOrderBookWindow, OutOfWindowRemovalDoesNotMoveTheWindow)
{
  constexpr size_t kLevels = 512;
  NLevelOrderBook<kLevels> book{Price::fromRaw(kTenthRaw)};

  {
    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(600000 * kTenthRaw, 1.0).ask(600002 * kTenthRaw, 1.0);
    book.applyBookUpdate(snap.event());
  }

  Update d(BookUpdateType::DELTA);
  d.bid(500000 * kTenthRaw, 0.0);
  book.applyBookUpdate(d.event());

  EXPECT_EQ(book.bestBid().value_or(Price{}).raw(), 600000 * kTenthRaw);
  EXPECT_EQ(book.bestAsk().value_or(Price{}).raw(), 600002 * kTenthRaw);
}

TEST(NLevelOrderBookWindow, DeltaAnchorsAnEmptyBook)
{
  NLevelOrderBook<512> book{Price::fromRaw(kTenthRaw)};

  Update d(BookUpdateType::DELTA);
  d.bid(600000 * kTenthRaw, 2.0).ask(600002 * kTenthRaw, 3.0);
  book.applyBookUpdate(d.event());

  EXPECT_EQ(book.bestBid().value_or(Price{}).raw(), 600000 * kTenthRaw);
  EXPECT_EQ(book.bestAsk().value_or(Price{}).raw(), 600002 * kTenthRaw);
}

// Emptying a side by delta left the side's index bounds at MAX_LEVELS. The next
// consume then read one element past the end of the array, which under a
// hardened standard library is an abort and otherwise silently folds a
// neighbouring field into the filled quantity.
TEST(NLevelOrderBookWindow, ConsumeAsksAfterDeltaEmptiedTheSide)
{
  NLevelOrderBook<1024> book{Price::fromRaw(kTenthRaw)};

  {
    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(1000 * kTenthRaw, 1.0).ask(1001 * kTenthRaw, 1.0);
    book.applyBookUpdate(snap.event());
  }
  {
    Update d(BookUpdateType::DELTA);
    d.ask(1001 * kTenthRaw, 0.0);
    book.applyBookUpdate(d.event());
  }
  ASSERT_FALSE(book.bestAsk().has_value());
  {
    Update d(BookUpdateType::DELTA);
    d.ask(1003 * kTenthRaw, 2.0);
    book.applyBookUpdate(d.event());
  }
  ASSERT_EQ(book.bestAsk().value_or(Price{}).raw(), 1003 * kTenthRaw);

  const auto [filled, notional] = book.consumeAsks(Quantity::fromDouble(10.0));
  EXPECT_EQ(filled.raw(), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(notional.raw(), Volume::fromDouble(200.6).raw());
}

TEST(NLevelOrderBookWindow, ConsumeBidsAfterDeltaEmptiedTheSide)
{
  NLevelOrderBook<1024> book{Price::fromRaw(kTenthRaw)};

  {
    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(1000 * kTenthRaw, 1.0).ask(1001 * kTenthRaw, 1.0);
    book.applyBookUpdate(snap.event());
  }
  {
    Update d(BookUpdateType::DELTA);
    d.bid(1000 * kTenthRaw, 0.0);
    book.applyBookUpdate(d.event());
  }
  ASSERT_FALSE(book.bestBid().has_value());
  {
    Update d(BookUpdateType::DELTA);
    d.bid(998 * kTenthRaw, 2.0);
    book.applyBookUpdate(d.event());
  }

  const auto [filled, notional] = book.consumeBids(Quantity::fromDouble(10.0));
  EXPECT_EQ(filled.raw(), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(notional.raw(), Volume::fromDouble(199.6).raw());
}

// Price scale is fixed at 1e8, so a 1e-8 tick is a raw divisor of 1 and a 2e-8
// tick a divisor of 2. Every level used to collapse onto index 1.
TEST(NLevelOrderBookWindow, TinyTickSizesKeepLevelsApart)
{
  for (int64_t tickRaw : {int64_t{1}, int64_t{2}, int64_t{3}, int64_t{4}})
  {
    NLevelOrderBook<1024> book{Price::fromRaw(tickRaw)};

    const int64_t bidTick = 1000 / tickRaw;
    const int64_t askTick = bidTick + 2;

    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(bidTick * tickRaw, 1.0).ask(askTick * tickRaw, 1.0);
    book.applyBookUpdate(snap.event());

    ASSERT_TRUE(book.bestBid().has_value()) << "tickRaw=" << tickRaw;
    EXPECT_EQ(book.bestBid()->raw(), bidTick * tickRaw) << "tickRaw=" << tickRaw;
    EXPECT_EQ(book.bestAsk()->raw(), askTick * tickRaw) << "tickRaw=" << tickRaw;
    EXPECT_FALSE(book.isCrossed()) << "tickRaw=" << tickRaw;
    EXPECT_EQ(book.spread()->raw(), 2 * tickRaw) << "tickRaw=" << tickRaw;
    EXPECT_EQ(book.mid()->raw(), (bidTick + askTick) * tickRaw / 2) << "tickRaw=" << tickRaw;
  }
}

// mid() rounded each side down to a whole tick before averaging, so an odd raw
// tick lost half a tick per side: -20% at a raw tick of 5, -100% at 1.
TEST(NLevelOrderBookWindow, MidIsExactForOddRawTickSizes)
{
  {
    NLevelOrderBook<1024> book{Price::fromRaw(5)};
    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(1000, 1.0).ask(1010, 1.0);
    book.applyBookUpdate(snap.event());
    ASSERT_TRUE(book.mid().has_value());
    EXPECT_EQ(book.mid()->raw(), 1005);
  }
  {
    NLevelOrderBook<1024> book{Price::fromRaw(3)};
    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(300, 1.0).ask(600, 1.0);
    book.applyBookUpdate(snap.event());
    ASSERT_TRUE(book.mid().has_value());
    EXPECT_EQ(book.mid()->raw(), 450);
  }
}

// The book's own mid() and SymbolContext::mid() answer the same question, and
// the C and Python surfaces read the book's. They used to disagree by 20%.
TEST(NLevelOrderBookWindow, MidMatchesTheAverageOfBothSides)
{
  for (int64_t tickRaw : {int64_t{1}, int64_t{3}, int64_t{5}, int64_t{25}, int64_t{10000000}})
  {
    NLevelOrderBook<1024> book{Price::fromRaw(tickRaw)};

    Update snap(BookUpdateType::SNAPSHOT);
    snap.bid(200 * tickRaw, 1.0).ask(203 * tickRaw, 1.0);
    book.applyBookUpdate(snap.event());

    ASSERT_TRUE(book.mid().has_value()) << "tickRaw=" << tickRaw;
    const int64_t expected = (book.bestBid()->raw() + book.bestAsk()->raw()) / 2;
    EXPECT_EQ(book.mid()->raw(), expected) << "tickRaw=" << tickRaw;
  }
}

// A zero tick size divided by zero inside the reciprocal and produced a book
// that was dead for the rest of its life without a single diagnostic.
TEST(NLevelOrderBookWindow, RejectsNonPositiveTickSize)
{
  EXPECT_THROW((NLevelOrderBook<64>{Price::fromRaw(0)}), std::invalid_argument);
  EXPECT_THROW((NLevelOrderBook<64>{Price{}}), std::invalid_argument);
  EXPECT_THROW((NLevelOrderBook<64>{Price::fromRaw(-1)}), std::invalid_argument);
  EXPECT_THROW((NLevelOrderBook<64>{Price::fromDouble(-0.01)}), std::invalid_argument);
  EXPECT_NO_THROW((NLevelOrderBook<64>{Price::fromRaw(1)}));
}
