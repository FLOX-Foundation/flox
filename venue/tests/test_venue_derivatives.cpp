/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/liquidation_monitor.h"
#include "flox-venue/matching_engine.h"
#include "flox/book/matching_book.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

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

constexpr SymbolId SYM = 1;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg(TriggerRef ref = TriggerRef::Last)
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.triggerRef = ref;
  return c;
}

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
  int triggers() const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (std::get_if<OrderTriggered>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  bool tradedBy(OrderId taker, double price) const
  {
    for (auto& e : ev)
    {
      if (auto* t = std::get_if<Trade>(&e))
      {
        if (t->takerId == taker && t->price == px(price))
        {
          return true;
        }
      }
    }
    return false;
  }
};

void test_mark_trigger()
{
  std::printf("test_mark_trigger\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(TriggerRef::Mark), cap.sink());
  eng.submit(limit(1, Side::SELL, 105, 10, 1));  // liquidity for the stop to hit

  NewOrder st;
  st.id = 2;
  st.symbol = SYM;
  st.side = Side::BUY;
  st.type = OrderType::STOP_MARKET;
  st.quantity = qty(3);
  st.triggerPrice = px(105);
  st.accountId = 2;
  eng.submit(st);
  CHECK(cap.triggers() == 0);  // no mark yet -> pending

  eng.setMarkPrice(px(104));  // below trigger
  CHECK(cap.triggers() == 0);

  eng.setMarkPrice(px(105));  // mark hits trigger -> fires (independent of last trade)
  CHECK(cap.triggers() == 1);
  CHECK(cap.tradedBy(2, 105.0));
}

void test_liquidation()
{
  std::printf("test_liquidation\n");
  // A long position, tiny equity: underwater as the mark drops.
  LiquidationMonitor mon(SYM, /*mmFraction*/ 0.005, /*insurance*/ 1e9);
  mon.openPosition(/*account*/ 9, /*signedQty*/ +10.0, /*entry*/ 100.0, /*equity*/ 5.0);
  CHECK(mon.openPositions() == 1);

  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(limit(100, Side::BUY, 60, 20, 3));  // bids to absorb the forced sell

  const auto orders = mon.onMark(60.0);  // long entry 100 -> deeply underwater at 60
  CHECK(orders.size() == 1);
  CHECK(mon.openPositions() == 0);
  if (!orders.empty())
  {
    CHECK(orders[0].side == Side::SELL);
    CHECK(orders[0].quantity == qty(10));
    eng.submit(orders[0]);                    // route the liquidation order to matching
    CHECK(cap.tradedBy(orders[0].id, 60.0));  // filled against the bid
  }
}

}  // namespace

TEST(Derivatives, EngineSuite)
{
  test_mark_trigger();
  test_liquidation();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
