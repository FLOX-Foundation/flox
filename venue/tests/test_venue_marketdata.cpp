/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/itch_codec.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/workload.h"
#include "flox/book/ladder_book.h"
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

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}
LadderBook::Config ladderCfg()
{
  return LadderBook::Config{0, px(0.01).raw(), 16000, 1 << 20};
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

bool sameMd(const MdMessage& a, const MdMessage& b)
{
  return a.type == b.type && a.seq == b.seq && a.symbol == b.symbol && a.id == b.id &&
         a.side == b.side && a.price == b.price && a.qty == b.qty && a.makerId == b.makerId;
}

void test_l2_and_codec()
{
  std::printf("test_l2_and_codec\n");
  std::vector<MdMessage> feed;
  MarketDataPublisher<> md([&](const MdMessage& m)
                           { feed.push_back(m); }, px(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e); });

  eng.submit(limit(1, Side::SELL, 100, 5));
  eng.submit(limit(2, Side::SELL, 101, 3));
  eng.submit(limit(3, Side::BUY, 99, 4));
  CHECK(md.book().bestAsk() == px(100));
  CHECK(md.book().askAtPrice(px(100)) == qty(5));
  CHECK(md.book().askAtPrice(px(101)) == qty(3));
  CHECK(md.book().bestBid() == px(99));
  CHECK(md.book().bidAtPrice(px(99)) == qty(4));

  eng.submit(limit(4, Side::BUY, 100, 2));  // crosses id1: trade 2 @100, id1 leaves 3
  CHECK(md.book().askAtPrice(px(100)) == qty(3));

  eng.submit(CancelOrder{2, SYM, 1});  // remove the 101 level
  CHECK(md.book().askAtPrice(px(101)) == qty(0));

  eng.submit(ModifyOrder{1, SYM, px(102), qty(3), 1});  // reprice 100->102
  CHECK(md.book().askAtPrice(px(100)) == qty(0));
  CHECK(md.book().bestAsk() == px(102));
  CHECK(md.book().askAtPrice(px(102)) == qty(3));

  // ITCH round-trip on the whole feed
  std::vector<uint8_t> buf;
  int rt = 0;
  for (const auto& m : feed)
  {
    ItchCodec::encode(m, buf);
    MdMessage back;
    CHECK(ItchCodec::decode(buf.data(), buf.size(), back));
    CHECK(sameMd(m, back));
    ++rt;
  }
  CHECK(rt > 0);
}

void test_book_agreement()
{
  std::printf("test_book_agreement\n");
  MarketDataPublisher<> md([](const MdMessage&) {}, px(0.01), SYM);
  MatchingEngine<LadderBook> eng(cfg(), [&](const OutboundEvent& e)
                                 { md.onEvent(e); }, LadderBook{ladderCfg()});

  workload::Params p;
  p.symbol = SYM;
  p.count = 300'000;
  const auto cmds = workload::symmetricLimits(p);

  // Full-depth agreement: compare EVERY price level's aggregate, not just the
  // touch -- MD drift at deeper levels (from modify / partial-fill / cancel)
  // would be invisible to a top-of-book-only check. Sampled periodically (a
  // full-depth walk every command over 300k would be too slow) plus always at
  // the touch.
  int mismatches = 0;
  std::vector<std::pair<Price, Quantity>> bl, al;
  for (size_t k = 0; k < cmds.size(); ++k)
  {
    eng.submit(cmds[k]);
    if (eng.book().bestBid() != md.book().bestBid() ||
        eng.book().bestAsk() != md.book().bestAsk())
    {
      ++mismatches;
    }
    if ((k % 500) == 0)  // periodic full-depth reconciliation
    {
      eng.book().levels(Side::BUY, bl);
      eng.book().levels(Side::SELL, al);
      for (const auto& [p, q] : bl)
      {
        if (md.book().bidAtPrice(p) != q)
        {
          ++mismatches;
        }
      }
      for (const auto& [p, q] : al)
      {
        if (md.book().askAtPrice(p) != q)
        {
          ++mismatches;
        }
      }
    }
  }
  CHECK(mismatches == 0);  // the feed never drifts from the engine book, at any level
  std::printf("  checked %zu commands (top + periodic full-depth), %d mismatches\n", cmds.size(),
              mismatches);
}

// The public depth must show only an iceberg's displayed peak, never its hidden
// reserve -- otherwise the book could be probed to reveal iceberg size.
void test_iceberg_hidden_from_md()
{
  std::printf("test_iceberg_hidden_from_md\n");
  std::vector<MdMessage> feed;
  std::vector<OrderExecuted> execs;
  MarketDataPublisher<> md([&](const MdMessage& m)
                           { feed.push_back(m); }, px(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(),
                                   [&](const OutboundEvent& e)
                                   {
                                     if (auto* x = std::get_if<OrderExecuted>(&e))
                                     {
                                       execs.push_back(*x);
                                     }
                                     md.onEvent(e);
                                   });

  NewOrder ice = limit(1, Side::SELL, 100, 20, 1);
  ice.visibleQuantity = qty(3);  // 20 total, only 3 displayed
  eng.submit(ice);
  // Public depth at 100 shows only the 3-lot peak, not the 20 total.
  CHECK(md.book().askAtPrice(px(100)) == qty(3));
  // The AddOrder message also carried the peak, not the reserve.
  bool sawAdd = false;
  for (const auto& m : feed)
  {
    if (m.type == MdType::AddOrder && m.id == 1)
    {
      sawAdd = true;
      CHECK(m.qty == qty(3));
    }
  }
  CHECK(sawAdd);

  // A plain (non-iceberg) order still shows its full size.
  eng.submit(limit(2, Side::SELL, 101, 7, 1));
  CHECK(md.book().askAtPrice(px(101)) == qty(7));

  // Refill: a buy lifts the whole 3-lot peak. The iceberg refills a fresh 3-lot
  // peak from its reserve -> public depth must still show 3 (17 remain hidden),
  // never the total. This checks the Executed-driven MD path, not just Accept.
  eng.submit(limit(3, Side::BUY, 100, 3, 2));
  CHECK(md.book().askAtPrice(px(100)) == qty(3));  // refilled peak, reserve stays hidden

  // PARTIAL peak fill (aggressor smaller than the peak): buy 2 of the 3-lot peak.
  // The peak is not exhausted, so no refill and no fresh Accept -- only Executed.
  // Public depth must drop to the new displayed peak (1), and the public Executed
  // message must publish the DISPLAYED remaining (1), never the whole 15
  // (1 shown + 14 hidden) -- else the hidden reserve leaks and depth underflows.
  execs.clear();
  eng.submit(limit(4, Side::BUY, 100, 2, 2));
  CHECK(md.book().askAtPrice(px(100)) == qty(1));  // displayed peak, not 15
  Quantity lastPubExec{};
  bool sawExec = false;
  for (const auto& m : feed)
  {
    if (m.type == MdType::Executed && m.id == 1)
    {
      sawExec = true;
      lastPubExec = m.qty;
    }
  }
  CHECK(sawExec);
  CHECK(lastPubExec == qty(1));  // public feed shows displayed remaining, not the reserve
  // The OWNER's exec report still reports the WHOLE remaining (1 + 14 = 15).
  bool sawMakerExec = false;
  for (const auto& x : execs)
  {
    if (x.id == 1 && !x.aggressor)
    {
      sawMakerExec = true;
      CHECK(x.leavesQty == qty(15));     // whole remaining for the owner
      CHECK(x.displayLeaves == qty(1));  // displayed peak for the public feed
    }
  }
  CHECK(sawMakerExec);
}

}  // namespace

TEST(Marketdata, EngineSuite)
{
  test_l2_and_codec();
  test_book_agreement();
  test_iceberg_hidden_from_md();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
