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

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.luldBps = 500;                  // +/- 5%
  c.luldHaltNs = DurationNs{1000};  // 1000 ns pause
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
  int rejects(RejectReason r) const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (auto* x = std::get_if<OrderRejected>(&e); x && x->reason == r)
      {
        ++n;
      }
    }
    return n;
  }
};

}  // namespace

TEST(Luld, EngineSuite)
{
  std::printf("test_luld\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // trade @100 -> last=100, band [95,105]
  CHECK(cap.trades() == 1);

  eng.submit(InboundCommand{limit(3, Side::BUY, 110, 1, 2)}, 2);  // 110 > 105 -> breach
  CHECK(cap.rejects(RejectReason::LuldBreach) == 1);

  eng.submit(InboundCommand{limit(4, Side::BUY, 100, 1, 2)}, 5);  // still in the pause
  CHECK(cap.rejects(RejectReason::Halted) == 1);

  eng.submit(InboundCommand{limit(5, Side::BUY, 100, 1, 2)}, 2000);  // pause elapsed -> resumes
  CHECK(cap.trades() == 2);                                          // trades against the resting ask @100

  // High-priced instrument + wide band: last raw = 1e7 * 1e8 = 1e15, luldBps 9999.
  // The old `lastPrice_.raw() * luldBps` (int64) = 1e15 * 9999 ~ 1e19 overflows
  // INT64_MAX (~9.2e18) -> signed-overflow UB and a garbage band. The 128-bit
  // computation must yield a correct band: [1000, ~1.9999e7].
  {
    Cap hc;
    SymbolConfig c;
    c.id = SYM;
    c.tickSize = px(0.01);
    c.minPrice = px(1.0);
    c.maxPrice = px(5e7);
    c.luldBps = 9999;  // +/- 99.99%
    c.luldHaltNs = DurationNs{1000};
    MatchingEngine<MatchingBook> heng(c, hc.sink());
    heng.submit(InboundCommand{limit(1, Side::SELL, 1e7, 5, 1)}, 0);
    heng.submit(InboundCommand{limit(2, Side::BUY, 1e7, 3, 2)}, 1);  // trade @1e7 -> last=1e7
    CHECK(hc.trades() == 1);
    // Within the band (upper ~1.9999e7): must NOT breach.
    heng.submit(InboundCommand{limit(3, Side::BUY, 1.5e7, 1, 2)}, 2);
    CHECK(hc.rejects(RejectReason::LuldBreach) == 0);
    // Above the band but below maxPrice: must breach (proves band is correct,
    // not a garbage value from the overflow).
    heng.submit(InboundCommand{limit(4, Side::BUY, 3e7, 1, 2)}, 3);
    CHECK(hc.rejects(RejectReason::LuldBreach) == 1);
  }

  // A MARKET order has no limit to gate it pre-trade, so it can sweep the book
  // and print outside the band. The fills stand, but the breaching print trips
  // the volatility pause, so the next order is halted -- market orders no longer
  // bypass LULD.
  {
    Cap mc;
    MatchingEngine<MatchingBook> meng(cfg(), mc.sink());  // band +/-5%
    meng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 0);
    meng.submit(InboundCommand{limit(2, Side::BUY, 100, 5, 2)}, 1);  // trade @100, ask side empty
    CHECK(mc.trades() == 1);                                         // band [95,105]
    // Only liquidity left is above the band; a market buy sweeps it and prints @108.
    meng.submit(InboundCommand{limit(3, Side::SELL, 108, 5, 1)}, 2);
    NewOrder mkt = limit(4, Side::BUY, 0, 2, 2);
    mkt.type = OrderType::MARKET;
    meng.submit(InboundCommand{mkt}, 3);                             // fills @108 -> last=108 > 105
    CHECK(mc.trades() == 2);                                         // the sweep printed
    meng.submit(InboundCommand{limit(5, Side::BUY, 100, 1, 2)}, 4);  // within the pause
    CHECK(mc.rejects(RejectReason::Halted) == 1);                    // market sweep tripped LULD
  }

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
