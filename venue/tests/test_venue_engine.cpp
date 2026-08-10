/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-venue/matching_engine.h"

#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 900;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount base(double v) { return amountOf(qty(v)); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }
int64_t i64(Amount a) { return static_cast<int64_t>(a); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1);
  c.maxPrice = px(1000);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  return c;
}

NewOrder limitOrder(OrderId id, Side s, double p, double q, uint64_t acct)
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
}  // namespace

// End-to-end venue: orders match through the engine, settle against the ledger,
// and value is conserved per asset (the property that makes it a real venue).
TEST(VenueEngine, MatchSettlesAndConservesValue)
{
  Ledger led;
  led.deposit(1, BASE, base(10));       // seller
  led.deposit(2, QUOTE, quote(10000));  // buyer
  const Amount initBase = led.total(1, BASE) + led.total(2, BASE);
  const Amount initQuote = led.total(1, QUOTE) + led.total(2, QUOTE);

  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);

  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 5, 1)}, 0);
  eng.submit(InboundCommand{limitOrder(2, Side::BUY, 100, 3, 2)}, 1);  // trades 3 @ 100

  EXPECT_EQ(i64(led.available(2, BASE)), i64(base(3)));      // buyer got base
  EXPECT_EQ(i64(led.available(1, QUOTE)), i64(quote(300)));  // seller got quote
  // Conservation per asset across both accounts + the venue fee account.
  EXPECT_EQ(i64(led.total(1, BASE) + led.total(2, BASE) + led.total(VENUE_ACCT, BASE)),
            i64(initBase));
  EXPECT_EQ(i64(led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE_ACCT, QUOTE)),
            i64(initQuote));
}

// Cancelling the unfilled remainder must return the reservation to available,
// leaving no buying power stranded in `reserved`.
TEST(VenueEngine, CancelReleasesReservation)
{
  Ledger led;
  led.deposit(1, BASE, base(10));
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE_ACCT);

  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 4, 1)}, 0);
  EXPECT_EQ(i64(led.reserved(1, BASE)), i64(base(4)));

  eng.submit(InboundCommand{CancelOrder{1, SYM, 1}}, 1);
  EXPECT_EQ(i64(led.reserved(1, BASE)), 0);
  EXPECT_EQ(i64(led.available(1, BASE)), i64(base(10)));
}

// Deterministic recovery: replaying the journal into a fresh engine reproduces
// the event stream bit-for-bit and the same ledger state.
TEST(VenueEngine, JournalReplayIsDeterministic)
{
  const std::string path = "/tmp/flox_venue_journal_test.bin";
  std::vector<std::pair<int64_t, InboundCommand>> cmds{
      {10, InboundCommand{limitOrder(1, Side::SELL, 100, 5, 1)}},
      {20, InboundCommand{limitOrder(2, Side::BUY, 100, 3, 2)}},
      {30, InboundCommand{CancelOrder{1, SYM, 1}}},
  };

  auto run = [&](Ledger& led, const std::vector<std::pair<int64_t, InboundCommand>>& in)
  {
    led.deposit(1, BASE, base(10));
    led.deposit(2, QUOTE, quote(10000));
    uint64_t h = 1469598103934665603ULL;
    MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                     { h = hashEvent(h, e); });
    eng.setLedger(&led, VENUE_ACCT);
    for (const auto& [ts, c] : in)
    {
      eng.submit(c, ts);
    }
    return h;
  };

  // Live run, journalling every command.
  Ledger liveLed;
  {
    Journal j(path);
    for (const auto& [ts, c] : cmds)
    {
      j.append(c, ts);
    }
    j.flush();
  }
  const uint64_t liveHash = run(liveLed, cmds);

  // Recovery run from the journal.
  const auto replayed = Journal::loadTimed(path);
  ASSERT_EQ(replayed.size(), cmds.size());
  Ledger recLed;
  const uint64_t recHash = run(recLed, replayed);

  EXPECT_EQ(recHash, liveHash);  // identical event stream
  EXPECT_EQ(i64(recLed.available(2, BASE)), i64(liveLed.available(2, BASE)));
  EXPECT_EQ(i64(recLed.available(1, QUOTE)), i64(liveLed.available(1, QUOTE)));
  std::remove(path.c_str());
}
