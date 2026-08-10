/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/symbol_router.h"
#include "flox-venue/workload.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;

void checkImpl(bool ok, const char* expr, const char* file, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL %s:%d  %s\n", file, line, expr);
  }
}
#define CHECK(x) checkImpl((x), #x, __FILE__, __LINE__)

SymbolConfig cfgFor(SymbolId id)
{
  SymbolConfig c;
  c.id = id;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  return c;
}

uint64_t runHash(const std::vector<InboundCommand>& cmds, Journal* journal)
{
  uint64_t h = 1469598103934665603ULL;
  auto sink = [&](const OutboundEvent& e)
  { h = hashEvent(h, e); };
  MatchingEngine<MatchingBook> eng(cfgFor(1), sink);
  for (const auto& c : cmds)
  {
    if (journal)
    {
      journal->append(c);
    }
    eng.submit(c);
  }
  return h;
}

void test_journal_replay()
{
  std::printf("test_journal_replay\n");
  workload::Params p;
  p.symbol = 1;
  p.count = 5000;
  auto cmds = workload::symmetricLimits(p);
  // exercise the cancel record path too (some hit live orders, some don't)
  cmds.emplace_back(CancelOrder{5, 1, 1});
  cmds.emplace_back(CancelOrder{123, 1, 1});
  cmds.emplace_back(CancelOrder{999999, 1, 1});

  const std::string path = "/tmp/flox_test_venue_reliability_journal_test.bin";

  uint64_t hashLive;
  uint64_t journaled;
  {
    Journal journal(path);
    hashLive = runHash(cmds, &journal);
    journal.flush();
    journaled = journal.count();
  }

  CHECK(journaled == cmds.size());

  const auto replayed = Journal::load(path);
  CHECK(replayed.size() == cmds.size());

  const uint64_t hashReplay = runHash(replayed, nullptr);
  CHECK(hashLive == hashReplay);  // deterministic recovery
}

void test_sharding()
{
  std::printf("test_sharding\n");
  SymbolRouter<MatchingBook> router(4);

  int trades1 = 0;
  int trades2 = 0;
  router.addSymbol(cfgFor(1), [&](const OutboundEvent& e)
                   { if (std::get_if<Trade>(&e)){ ++trades1;
} });
  router.addSymbol(cfgFor(2), [&](const OutboundEvent& e)
                   { if (std::get_if<Trade>(&e)){ ++trades2;
} });

  CHECK(router.shardCount() == 4);
  CHECK(router.shardOf(1) == router.shardOf(1));  // deterministic
  CHECK(router.has(1) && router.has(2));

  auto mk = [](OrderId id, SymbolId sym, Side s, double px, double q)
  {
    NewOrder o;
    o.id = id;
    o.symbol = sym;
    o.side = s;
    o.type = OrderType::LIMIT;
    o.price = Price::fromDouble(px);
    o.quantity = Quantity::fromDouble(q);
    o.accountId = 1;
    return InboundCommand{o};
  };

  // Cross on symbol 1 only.
  router.submit(mk(1, 1, Side::SELL, 100.0, 5));
  router.submit(mk(2, 1, Side::BUY, 100.0, 5));
  CHECK(trades1 == 1);
  CHECK(trades2 == 0);  // symbol 2 untouched

  // Cross on symbol 2 only.
  router.submit(mk(3, 2, Side::SELL, 100.0, 5));
  router.submit(mk(4, 2, Side::BUY, 100.0, 5));
  CHECK(trades2 == 1);
  CHECK(trades1 == 1);  // symbol 1 unchanged
}

}  // namespace

TEST(Reliability, EngineSuite)
{
  test_journal_replay();
  test_sharding();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
