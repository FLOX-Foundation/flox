/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/ledger.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/symbol_router.h"
#include "flox/book/matching_book.h"

#include "flox/backtest/fee_schedule.h"

#include <gtest/gtest.h>
#include <array>
#include <cstdint>

#include <cstdio>
#include <cstdlib>
#include <memory>
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

constexpr AssetId QUOTE = 0;
constexpr uint64_t VENUE = 500;
constexpr int NACCT = 6;
constexpr int NSYM = 3;
// Symbol s uses base asset (s) and the shared quote asset.
constexpr std::array<SymbolId, NSYM> SYMS = {101, 102, 103};
constexpr std::array<AssetId, NSYM> BASES = {1, 2, 3};

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount base(double v) { return amountOf(qty(v)); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

struct Rng
{
  uint64_t s;
  uint64_t next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

SymbolConfig cfg(SymbolId id, AssetId baseAsset)
{
  SymbolConfig c;
  c.id = id;
  c.tickSize = px(0.01);
  c.minPrice = px(50);
  c.maxPrice = px(150);
  c.baseAsset = baseAsset;
  c.quoteAsset = QUOTE;
  return c;
}

void test_soak()
{
  std::printf("test_multi_symbol_soak\n");
  Ledger led;
  for (int a = 1; a <= NACCT; ++a)
  {
    led.deposit(a, QUOTE, quote(1000000));
    for (int s = 0; s < NSYM; ++s)
    {
      led.deposit(a, BASES[s], base(10000));
    }
  }
  const Amount initQuote = quote(1000000) * NACCT;
  const Amount initBase = base(10000) * NACCT;

  Metrics metrics;
  // One market-data publisher per symbol; kept in a stable container.
  std::vector<std::unique_ptr<MarketDataPublisher<>>> mds;
  for (int s = 0; s < NSYM; ++s)
  {
    mds.push_back(std::make_unique<MarketDataPublisher<>>([](const MdMessage&) {}, px(0.01), SYMS[s]));
  }

  SymbolRouter<MatchingBook> router(NSYM);
  std::array<MatchingEngine<MatchingBook>*, NSYM> engines{};
  for (int s = 0; s < NSYM; ++s)
  {
    MarketDataPublisher<>* md = mds[s].get();
    auto& eng = router.addSymbol(cfg(SYMS[s], BASES[s]), [&metrics, md](const OutboundEvent& e)
                                 {
                                   metrics.observe(e);
                                   md->onEvent(e); });
    flox::FeeSchedule fs;
    fs.addTier(0.0, -1.0, 2.0);  // maker rebate / taker fee -> venue nets fees
    eng.setFeeSchedule(fs);
    eng.setLedger(&led, VENUE);
    engines[s] = &eng;
  }

  auto assetConserved = [&](AssetId asset, Amount init)
  {
    Amount t = led.total(VENUE, asset);
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, asset);
    }
    return t == init;
  };

  Rng rng{0x5EED1234ABULL};
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  std::array<OrderId, NSYM> nextId{};
  for (int s = 0; s < NSYM; ++s)
  {
    nextId[s] = 1;
  }
  int breaches = 0;

  const int OPS = 150000;
  for (int i = 0; i < OPS; ++i)
  {
    const uint64_t r = rng.next();
    const int s = static_cast<int>((r >> 3) % NSYM);
    const uint32_t kind = r % 100;
    if (kind < 15 && nextId[s] > 1)
    {
      const OrderId victim = 1 + (rng.next() % (nextId[s] - 1));
      // encode a per-symbol unique id space: id = sym-local; router uses symbol
      router.submit(InboundCommand{CancelOrder{victim + static_cast<OrderId>(s) * 10000000, SYMS[s], 0}});
    }
    else
    {
      NewOrder o;
      o.id = nextId[s]++ + static_cast<OrderId>(s) * 10000000;
      o.symbol = SYMS[s];
      o.side = (r & 1) ? Side::BUY : Side::SELL;
      o.accountId = 1 + (r >> 8) % NACCT;
      const int ticks = static_cast<int>((r >> 1) % 101) - 50;
      o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
      o.quantity = qty(1.0 + static_cast<double>((r >> 20) % 5));
      o.type = (kind < 25) ? OrderType::MARKET : OrderType::LIMIT;
      router.submit(InboundCommand{o});
    }

    if (!assetConserved(QUOTE, initQuote))
    {
      ++breaches;
    }
    for (int k = 0; k < NSYM; ++k)
    {
      if (!assetConserved(BASES[k], initBase))
      {
        ++breaches;
      }
    }
    if (breaches == 1)
    {
      std::printf("  first breach at op %d\n", i);
    }
  }

  CHECK(breaches == 0);
  CHECK(metrics.trades > 0);
  std::printf("  %d ops across %d symbols: trades=%llu, breaches=%d\n", OPS, NSYM,
              (unsigned long long)metrics.trades, breaches);

  // Market data agrees with each engine's book, and books are well-formed.
  for (int s = 0; s < NSYM; ++s)
  {
    CHECK(engines[s]->book().bestBid() == mds[s]->book().bestBid());
    CHECK(engines[s]->book().bestAsk() == mds[s]->book().bestAsk());
    const auto bb = engines[s]->book().bestBid();
    const auto ba = engines[s]->book().bestAsk();
    if (bb.has_value() && ba.has_value())
    {
      CHECK(bb.value() < ba.value());
    }
  }

  // Cross-shard account snapshot: the router aggregates each symbol where an
  // account is active, and the total resting-order count across the router's
  // snapshots equals the sum of every engine's resting orders.
  uint64_t routerResting = 0;
  for (int a = 1; a <= NACCT; ++a)
  {
    for (const auto& [sym, snap] : router.snapshotAccount(a))
    {
      (void)sym;
      routerResting += snap.openOrders.size();
    }
  }
  uint64_t engineResting = 0;
  for (int s = 0; s < NSYM; ++s)
  {
    engineResting += engines[s]->restingOrderCount();
  }
  CHECK(routerResting == engineResting);  // snapshots account for every live order
}

}  // namespace

TEST(MultiSymbolSoak, EngineSuite)
{
  test_soak();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
