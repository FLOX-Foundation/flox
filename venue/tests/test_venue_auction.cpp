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
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  return c;
}
LadderBook::Config lc() { return LadderBook::Config{0, px(0.01).raw(), 200000, 1 << 16}; }

NewOrder limit(OrderId id, Side s, double p, double q)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = id;
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
  Quantity tradedQty() const
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
  bool allTradesAt(double p) const
  {
    for (auto& e : ev)
    {
      if (auto* x = std::get_if<Trade>(&e))
      {
        if (!(x->price == px(p)))
        {
          return false;
        }
      }
    }
    return true;
  }
};

template <class Book>
void run(const std::function<Book()>& mk, const char* label)
{
  g_label = label;
  std::printf("== auction book: %s ==\n", label);
  Cap cap;
  MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
  eng.beginPreOpen();
  eng.submit(InboundCommand{limit(1, Side::BUY, 101, 5)}, 0);
  eng.submit(InboundCommand{limit(2, Side::SELL, 99, 5)}, 1);
  eng.submit(InboundCommand{limit(3, Side::BUY, 100, 3)}, 2);
  eng.submit(InboundCommand{limit(4, Side::SELL, 100, 2)}, 3);
  CHECK(cap.trades() == 0);  // no matching during pre-open

  eng.openContinuous();  // uncross
  // demand/supply is maximized at 100 -> 7 units executed, all at 100
  CHECK(cap.tradedQty() == qty(7));
  CHECK(cap.allTradesAt(100.0));

  // residual: bid id3 has 1 left @100; continuous trading resumes
  CHECK(eng.book().bestBid() == px(100));
  eng.submit(InboundCommand{limit(5, Side::SELL, 100, 1)}, 4);
  CHECK(cap.tradedQty() == qty(8));  // +1

  {  // An uncross must not print an account against itself. There is no
     // aggressor in an auction, so the mode is read off each resting order and
     // applied from its owner's point of view: the counterparty is the
     // "oldest" leg, the requester's own order is the "newest".
    Cap c2;
    MatchingEngine<Book> e2(cfg(), c2.sink(), mk());
    e2.beginPreOpen();
    NewOrder b = limit(11, Side::BUY, 100, 5);
    b.accountId = 42;
    b.stp = STPMode::CancelOldest;  // kill the counterparty, keep mine
    NewOrder a = limit(12, Side::SELL, 100, 5);
    a.accountId = 42;
    e2.submit(InboundCommand{b}, 0);
    e2.submit(InboundCommand{a}, 1);
    e2.openContinuous();
    CHECK(c2.trades() == 0);               // no wash print
    CHECK(e2.book().find(12) == nullptr);  // counterparty pulled
    CHECK(e2.book().find(11) != nullptr);  // requester survives
  }
  {  // CancelNewest from the requester pulls its own order and leaves the
     // counterparty resting.
    Cap c2;
    MatchingEngine<Book> e2(cfg(), c2.sink(), mk());
    e2.beginPreOpen();
    NewOrder b = limit(11, Side::BUY, 100, 5);
    b.accountId = 42;
    b.stp = STPMode::CancelNewest;
    NewOrder a = limit(12, Side::SELL, 100, 5);
    a.accountId = 42;
    e2.submit(InboundCommand{b}, 0);
    e2.submit(InboundCommand{a}, 1);
    e2.openContinuous();
    CHECK(c2.trades() == 0);
    CHECK(e2.book().find(11) == nullptr);
    CHECK(e2.book().find(12) != nullptr);
  }
  {  // Decrement trims both legs by the overlap; the larger leg keeps its
     // remainder and a stranger can still trade with it.
    Cap c2;
    MatchingEngine<Book> e2(cfg(), c2.sink(), mk());
    e2.beginPreOpen();
    NewOrder b = limit(11, Side::BUY, 100, 5);
    b.accountId = 42;
    b.stp = STPMode::Decrement;
    NewOrder a = limit(12, Side::SELL, 100, 2);
    a.accountId = 42;
    e2.submit(InboundCommand{b}, 0);
    e2.submit(InboundCommand{a}, 1);
    e2.openContinuous();
    CHECK(c2.trades() == 0);
    CHECK(e2.book().find(12) == nullptr);         // smaller leg gone
    CHECK(e2.book().find(11)->leaves == qty(3));  // 5 - 2 left resting
  }
  {  // Two different accounts of one firm are the same trader for this
     // purpose, and an order with no mode still prints normally.
    Cap c2;
    MatchingEngine<Book> e2(cfg(), c2.sink(), mk());
    e2.setStpGroup(42, 900);
    e2.setStpGroup(43, 900);
    e2.beginPreOpen();
    NewOrder b = limit(11, Side::BUY, 100, 5);
    b.accountId = 42;
    b.stp = STPMode::CancelOldest;
    NewOrder a = limit(12, Side::SELL, 100, 5);
    a.accountId = 43;  // same firm
    NewOrder a2 = limit(13, Side::SELL, 100, 5);
    a2.accountId = 77;  // a stranger
    e2.submit(InboundCommand{b}, 0);
    e2.submit(InboundCommand{a}, 1);
    e2.submit(InboundCommand{a2}, 2);
    e2.openContinuous();
    CHECK(e2.book().find(12) == nullptr);  // same-firm leg pulled
    CHECK(c2.tradedQty() == qty(5));       // the stranger fills instead
  }
  {  // Without a mode nothing changes: an account that never asked for
     // prevention still crosses itself, exactly as continuous trading does.
    Cap c2;
    MatchingEngine<Book> e2(cfg(), c2.sink(), mk());
    e2.beginPreOpen();
    NewOrder b = limit(11, Side::BUY, 100, 5);
    b.accountId = 42;
    NewOrder a = limit(12, Side::SELL, 100, 5);
    a.accountId = 42;
    e2.submit(InboundCommand{b}, 0);
    e2.submit(InboundCommand{a}, 1);
    e2.openContinuous();
    CHECK(c2.tradedQty() == qty(5));
  }
}

}  // namespace

TEST(Auction, EngineSuite)
{
  run<MatchingBook>([]
                    { return MatchingBook{}; }, "map-reference");
  run<LadderBook>([]
                  { return LadderBook{lc()}; }, "ladder-o1");
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
