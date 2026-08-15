/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
const char* g_label = "";
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL [%s] line %d: %s\n", g_label, line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}
LadderBook::Config lc() { return LadderBook::Config{0, px(0.01).raw(), 16000, 1 << 16}; }

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct = 1)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

struct Cap
{
  std::vector<OutboundEvent> ev;
  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }
  int trades() const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (std::get_if<Trade>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  Quantity forMaker(OrderId m) const
  {
    Quantity t{};
    for (auto& e : ev)
    {
      if (auto* x = std::get_if<Trade>(&e); x && x->makerId == m)
      {
        t += x->quantity;
      }
    }
    return t;
  }
  Quantity total() const
  {
    Quantity t{};
    for (auto& e : ev)
    {
      if (auto* x = std::get_if<Trade>(&e))
      {
        t += x->quantity;
      }
    }
    return t;
  }
};

template <class Book>
void run(const std::function<Book()>& mk, const char* label)
{
  g_label = label;
  std::printf("== pro-rata book: %s ==\n", label);
  {  // partial: BUY 5 split across a 2/3/5 level (total 10) -> 1.0 / 1.5 / 2.5
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    eng.submit(limit(1, Side::SELL, 100, 2, 1));
    eng.submit(limit(2, Side::SELL, 100, 3, 1));
    eng.submit(limit(3, Side::SELL, 100, 5, 1));
    eng.submit(limit(9, Side::BUY, 100, 5, 2));
    CHECK(cap.trades() == 3);
    CHECK(cap.total() == qty(5));
    CHECK(cap.forMaker(1) == qty(1.0));
    CHECK(cap.forMaker(2) == qty(1.5));
    CHECK(cap.forMaker(3) == qty(2.5));
  }
  {  // full sweep: BUY 10 clears the level, everyone fully filled
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    eng.submit(limit(1, Side::SELL, 100, 2, 1));
    eng.submit(limit(2, Side::SELL, 100, 3, 1));
    eng.submit(limit(3, Side::SELL, 100, 5, 1));
    eng.submit(limit(9, Side::BUY, 100, 10, 2));
    CHECK(cap.trades() == 3);
    CHECK(cap.total() == qty(10));
    CHECK(eng.book().empty());
  }
  {  // pro-rata over displayed size with an iceberg maker present
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    NewOrder ice = limit(1, Side::SELL, 100, 10, 1);  // total 10
    ice.visibleQuantity = qty(4);                     // displayed 4, hidden 6
    eng.submit(ice);
    eng.submit(limit(2, Side::SELL, 100, 4, 1));  // plain displayed 4
    eng.submit(limit(9, Side::BUY, 100, 4, 2));   // 4 pro-rata over displayed 4/4 -> 2/2
    CHECK(cap.trades() == 2);
    CHECK(cap.forMaker(1) == qty(2));
    CHECK(cap.forMaker(2) == qty(2));
  }
  {  // full sweep drains the iceberg reserve + plain order
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    NewOrder ice = limit(1, Side::SELL, 100, 10, 1);
    ice.visibleQuantity = qty(4);
    eng.submit(ice);
    eng.submit(limit(2, Side::SELL, 100, 6, 1));
    eng.submit(limit(9, Side::BUY, 100, 16, 2));  // sweep all 10 + 6
    CHECK(cap.total() == qty(16));
    CHECK(eng.book().empty());
  }
  {  // defensive: a lastLook maker planted PAST validate() is skipped, not
     // filled as firm -- pro-rata cannot hold a slice, so filling it would
     // fake firmness the maker never granted. The counter records the event;
     // the allocation over the firm participants is unchanged.
    Cap cap;
    Matcher<Book> m(MatchPolicy::ProRata);
    Book book = mk();
    RestingOrder ll{1, 1, px(100), qty(5), Side::SELL};
    ll.lastLook = true;  // only possible here by bypassing admission
    book.addResting(Side::SELL, ll);
    book.addResting(Side::SELL, RestingOrder{2, 1, px(100), qty(2), Side::SELL});
    book.addResting(Side::SELL, RestingOrder{3, 1, px(100), qty(3), Side::SELL});
    uint64_t seq = 0;
    const NewOrder agg = limit(9, Side::BUY, 100, 4, 2);
    const MatchOutcome out = m.cross(agg, book, [&]
                                     { return ++seq; }, cap.sink());
    CHECK(cap.forMaker(1).isZero());     // the LL maker never fills
    CHECK(cap.forMaker(2) == qty(1.6));  // 4 pro-rata over firm 2/3
    CHECK(cap.forMaker(3) == qty(2.4));
    CHECK(cap.total() == qty(4));
    CHECK(out.takerComplete);
    CHECK(m.skippedLastLookProRata() >= 1);  // defensive path counted
    CHECK(book.find(1) != nullptr);
    CHECK(book.find(1)->leaves == qty(5));  // LL maker untouched on the book
  }
  {  // defensive: a level that is ONLY lastLook liquidity stops the sweep --
     // the aggressor residual follows its TIF instead of faking a fill
    Cap cap;
    Matcher<Book> m(MatchPolicy::ProRata);
    Book book = mk();
    RestingOrder ll{1, 1, px(100), qty(5), Side::SELL};
    ll.lastLook = true;
    book.addResting(Side::SELL, ll);
    uint64_t seq = 0;
    const NewOrder agg = limit(9, Side::BUY, 100, 4, 2);
    const MatchOutcome out = m.cross(agg, book, [&]
                                     { return ++seq; }, cap.sink());
    CHECK(cap.trades() == 0);
    CHECK(out.leaves == qty(4));
    CHECK(out.residualRests);  // GTC residual rests as usual
    CHECK(m.skippedLastLookProRata() >= 1);
    CHECK(book.find(1)->leaves == qty(5));
  }
  {  // Self-trade prevention binds under pro-rata exactly as it does under
     // price-time. The allocation is a different way of choosing who fills,
     // not a different set of participants: an account must never be on both
     // sides of the same print whatever the policy is.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    eng.submit(limit(1, Side::SELL, 100, 4, 7));  // same account as the taker
    eng.submit(limit(2, Side::SELL, 100, 6, 8));  // a stranger at the same level
    NewOrder agg = limit(9, Side::BUY, 100, 10, 7);
    agg.stp = STPMode::CancelOldest;
    eng.submit(agg);
    CHECK(cap.forMaker(1).isZero());  // never trades with itself
    CHECK(cap.forMaker(2) == qty(6));
    CHECK(eng.book().find(1) == nullptr);  // CancelOldest pulled the resting leg
  }
  {  // CancelNewest under pro-rata cancels the aggressor and prints nothing,
     // even though strangers at the level would otherwise have filled.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    eng.submit(limit(1, Side::SELL, 100, 4, 7));
    eng.submit(limit(2, Side::SELL, 100, 6, 8));
    NewOrder agg = limit(9, Side::BUY, 100, 10, 7);
    agg.stp = STPMode::CancelNewest;
    eng.submit(agg);
    CHECK(cap.trades() == 0);
    CHECK(eng.book().find(1) != nullptr);  // the resting leg survives
    CHECK(eng.book().find(9) == nullptr);  // the aggressor does not rest
  }
  {  // Decrement trims both legs by the overlap without printing, and the
     // aggressor's remainder still fills against the stranger.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    eng.submit(limit(1, Side::SELL, 100, 4, 7));
    eng.submit(limit(2, Side::SELL, 100, 6, 8));
    NewOrder agg = limit(9, Side::BUY, 100, 10, 7);
    agg.stp = STPMode::Decrement;
    eng.submit(agg);
    CHECK(cap.forMaker(1).isZero());
    CHECK(cap.forMaker(2) == qty(6));  // 10 - 4 decremented = 6 left to trade
    CHECK(eng.book().find(1) == nullptr);
  }
  {  // The firm group is the STP scope under pro-rata too: two different
     // accounts in one group must not trade with each other.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk(), MatchPolicy::ProRata);
    eng.setStpGroup(7, 99);
    eng.setStpGroup(11, 99);
    eng.submit(limit(1, Side::SELL, 100, 4, 11));  // same firm, different account
    eng.submit(limit(2, Side::SELL, 100, 6, 8));
    NewOrder agg = limit(9, Side::BUY, 100, 10, 7);
    agg.stp = STPMode::CancelOldest;
    eng.submit(agg);
    CHECK(cap.forMaker(1).isZero());
    CHECK(cap.forMaker(2) == qty(6));
  }
}

}  // namespace

TEST(Prorata, EngineSuite)
{
  run<MatchingBook>([]
                    { return MatchingBook{}; }, "map-reference");
  run<LadderBook>([]
                  { return LadderBook{lc()}; }, "ladder-o1");
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
