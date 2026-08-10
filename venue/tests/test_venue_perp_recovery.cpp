/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <utility>
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
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 999;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1);
  c.maxPrice = px(1000);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.maintenanceMarginBps = 500;
  return c;
}
NewOrder ord(OrderId id, Side s, double p, double q, uint64_t acct)
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

// The session: open a long/short pair, apply funding, then crash the mark to
// force a maintenance-margin liquidation. Marks/funding are COMMANDS now.
std::vector<std::pair<int64_t, InboundCommand>> session()
{
  std::vector<std::pair<int64_t, InboundCommand>> s;
  s.emplace_back(0, InboundCommand{SetMark{SYM, px(100)}});
  s.emplace_back(1, InboundCommand{ord(1, Side::BUY, 100, 10, 1)});     // acct1 long 10 @ 100 (IM 100)
  s.emplace_back(2, InboundCommand{ord(2, Side::SELL, 100, 10, 2)});    // acct2 short
  s.emplace_back(3, InboundCommand{ApplyFunding{SYM, 0.01, px(100)}});  // longs pay
  s.emplace_back(4, InboundCommand{SetMark{SYM, px(91)}});              // still solvent
  s.emplace_back(5, InboundCommand{SetMark{SYM, px(85)}});              // acct1 liquidated here
  return s;
}

struct Run
{
  Ledger led;
  int64_t pos1{0}, pos2{0};
  Amount venueQuote{0};
  std::vector<uint64_t> liquidated;
};

Run drive(const std::vector<std::pair<int64_t, InboundCommand>>& cmds)
{
  Run r;
  r.led.deposit(1, QUOTE, quote(100));  // exactly IM -> highly leveraged
  r.led.deposit(2, QUOTE, quote(1000));
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     if (const auto* l = std::get_if<Liquidation>(&e)){
                                       r.liquidated.push_back(l->account);
} });
  eng.setLedger(&r.led, VENUE);
  for (const auto& [ts, c] : cmds)
  {
    eng.submit(c, ts);
  }
  r.pos1 = eng.positionQty(1);
  r.pos2 = eng.positionQty(2);
  return r;
}

void test_perp_recovery()
{
  std::printf("test_perp_recovery\n");
  const std::string path = "/tmp/flox_test_venue_perp_recovery_perp_recovery.bin";
  const auto cmds = session();

  // Live: journal every command (incl. SetMark / ApplyFunding).
  {
    Journal j(path);
    for (const auto& [ts, c] : cmds)
    {
      j.append(c, ts);
    }
    j.flush();
  }
  const Run live = drive(cmds);
  CHECK(live.pos1 == 0);  // acct1 was liquidated by the mark crash
  CHECK(!live.liquidated.empty());

  // Recover: replay ONLY what the journal holds. If marks/funding weren't
  // journaled, acct1 would still be open here and balances would diverge.
  const auto replayed = Journal::loadTimed(path);
  CHECK(replayed.size() == cmds.size());
  const Run rec = drive(replayed);

  CHECK(rec.pos1 == live.pos1 && rec.pos2 == live.pos2);       // positions reconstruct
  CHECK(rec.liquidated == live.liquidated);                    // same liquidation replayed
  CHECK(rec.led.total(1, QUOTE) == live.led.total(1, QUOTE));  // balances reconstruct
  CHECK(rec.led.total(2, QUOTE) == live.led.total(2, QUOTE));
  CHECK(rec.led.total(VENUE, QUOTE) == live.led.total(VENUE, QUOTE));
}

}  // namespace

TEST(PerpRecovery, EngineSuite)
{
  test_perp_recovery();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
