/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Hash-bucket order must not reach anything observable (engine half).
 *
 * The golden replay found StopBook::ids() publishing an emergency cancel's
 * conditionals in bucket order: same state on both standard libraries,
 * different event stream, and no test in the tree could see it because every
 * comparison in the suite compares state. The audit that followed (W32-T011)
 * walked every unordered container in venue/include to the place its order
 * either dies or becomes visible. These are the property tests for the places
 * where it became visible.
 *
 * Each test asserts the ORDER, not the contents -- the contents were already
 * correct, which is the whole difficulty. Each is red with the sort removed
 * from the accessor it covers, on the library it was written on; the same
 * assertion is what makes it red on the other one, where bucket order differs
 * again. That is why the assertion is `sorted`, not `equals this sequence`:
 * a recorded sequence would just be the golden table again, and the golden
 * table can only catch this on whichever library it was not recorded on.
 *
 * Inputs are deliberately many and scattered. Two or three keys can come out
 * of a hash map in ascending order by luck; sixteen scattered ones do not.
 *
 * The perimeter half (the FIX session sidecar) is in
 * test_venue_iteration_order_sidecar.cpp -- it needs headers the engine-only
 * build does not have. The split is not cosmetic: venue-engine-only configures
 * with FLOX_VENUE_PERIMETER=OFF, so a file that reaches for a perimeter header
 * is dropped from that job entirely, and these assertions have to keep running
 * in a configuration that does not build the gateways.
 */
#include "flox-venue/collateral.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/cross_margin.h"
#include "flox-venue/ledger.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/symbol_router.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
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

constexpr SymbolId SYM = 1;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 999;
constexpr uint64_t ACCT = 42;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

// Keys spread across the whole range rather than 1..N: contiguous small keys
// land in contiguous buckets, which is the one layout that can come out
// ascending by accident.
const std::vector<uint64_t> kScattered = {9007, 13, 480001, 226, 5, 78, 31337,
                                          1902, 64, 7, 250000, 4111, 88, 1500003,
                                          19, 6002};

SymbolConfig cfg(SymbolId id = SYM)
{
  SymbolConfig c;
  c.id = id;
  c.tickSize = px(0.01);
  c.minPrice = px(0.01);
  c.maxPrice = px(100000.0);
  c.quoteAsset = QUOTE;
  return c;
}

NewOrder bid(OrderId id, double p, double q, uint64_t acct, SymbolId sym = SYM)
{
  NewOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

// ---------------------------------------------------------------------------
// MatchingEngine::snapshotAccount -- the reconnect view.
//
// session_verbs.h answers AccountSnapshotRequest by encoding openOrders
// straight to the wire, one OrderAccepted frame per entry. The ids come from
// byAccount_'s per-account unordered_set, so without the sort the frame
// sequence a reconnecting client receives is a bucket-layout artifact.
void test_engine_snapshot_open_orders_sorted()
{
  std::printf("test_engine_snapshot_open_orders_sorted\n");
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  // All resting, none crossing: one price level would merge into one list, so
  // each order gets its own price and stays its own book entry.
  double p = 10.0;
  for (uint64_t id : kScattered)
  {
    eng.submit(bid(static_cast<OrderId>(id), p, 1.0, ACCT));
    p += 0.01;
  }

  const auto snap = eng.snapshotAccount(ACCT);
  CHECK(snap.openOrders.size() == kScattered.size());

  std::vector<OrderId> got;
  got.reserve(snap.openOrders.size());
  for (const auto& o : snap.openOrders)
  {
    got.push_back(o.id);
  }
  CHECK(std::is_sorted(got.begin(), got.end()));

  std::vector<OrderId> want(kScattered.begin(), kScattered.end());
  std::sort(want.begin(), want.end());
  CHECK(got == want);  // sorted AND complete: a sort that dropped an order is not a fix
}

// ---------------------------------------------------------------------------
// CollateralSchedule::forEachAsset -- the greedy liquidation-time consumer.
//
// Not a report order: CrossMarginManager::convertCollateral sells each asset
// only up to the quote deficit still outstanding and stops once it is covered.
// The enumeration order therefore decides WHICH coin a liquidating account's
// basket is sold from -- the balances left on the ledger, not the order of a
// line in a log.
void test_collateral_assets_sorted()
{
  std::printf("test_collateral_assets_sorted\n");
  CollateralSchedule sched;
  for (uint64_t a : kScattered)
  {
    sched.configure(static_cast<AssetId>(a), px(1.0).raw(), /*haircut*/ 0);
  }

  std::vector<AssetId> seen;
  sched.forEachAsset([&](AssetId a)
                     { seen.push_back(a); });
  CHECK(seen.size() == kScattered.size());
  CHECK(std::is_sorted(seen.begin(), seen.end()));
}

// The consequence, at the ledger: a bankrupt account holding several coins,
// each on its own worth more than the deficit, is sold out of the LOWEST
// AssetId it holds and no other. Without the sort the coin sold is whichever
// one the bucket layout offered first.
void test_collateral_conversion_picks_lowest_asset()
{
  std::printf("test_collateral_conversion_picks_lowest_asset\n");
  const Amount kPerCoin = amountOf(Volume::fromDouble(10.0));  // 10 units of each coin
  CollateralSchedule sched;
  sched.configure(QUOTE, px(1.0).raw(), 0);
  std::vector<AssetId> coins;
  for (size_t i = 0; i < kScattered.size(); ++i)
  {
    const auto id = static_cast<AssetId>(kScattered[i] % 4096 + 2);  // != QUOTE
    if (sched.accepts(id))
    {
      continue;
    }
    sched.configure(id, px(12.0).raw(), /*haircut*/ 0);  // 10 units -> 120 quote
    coins.push_back(id);
  }
  std::sort(coins.begin(), coins.end());

  Ledger led;
  std::vector<Liquidation> liqs;
  CrossMarginManager cm(led, QUOTE, VENUE, [&](const Liquidation& l)
                        { liqs.push_back(l); });
  cm.setCollateralSchedule(&sched);
  // Maintenance at full notional: the account is under water on a one-tick
  // move, which is what keeps the deficit small enough that ANY single coin
  // covers it while the whole basket is still thin enough to be liquidated.
  cm.configureSymbol(SYM, /*imBps*/ 10000, /*mmBps*/ 10000);

  for (AssetId c : coins)
  {
    led.deposit(ACCT, c, kPerCoin);
  }
  led.deposit(VENUE, QUOTE, quote(1'000'000));

  // Long 20 at 100, marked to 99: a 20-quote loss against a zero quote wallet,
  // so the liquidation has to sell coin before it can reach insurance. Every
  // coin is worth 120 -- six times the deficit -- so exactly ONE is touched,
  // and which one is the enumeration's answer.
  cm.applyFill(ACCT, SYM, Side::BUY, qty(20).raw(), px(100).raw());
  cm.setMark(SYM, px(99.0));

  CHECK(!liqs.empty());
  std::vector<AssetId> sold;
  for (AssetId c : coins)
  {
    if (led.total(ACCT, c) != kPerCoin)
    {
      sold.push_back(c);
    }
  }
  CHECK(sold.size() == 1);
  CHECK(!sold.empty() && sold.front() == coins.front());
}

// ---------------------------------------------------------------------------
// CrossMarginManager::liquidate -- one Liquidation per closed leg.
//
// The legs live in a per-account unordered_map keyed by symbol, so without the
// sort a multi-symbol liquidation publishes its legs in bucket order: same
// wallet, same closed positions, a different event stream.
void test_cross_margin_liquidation_legs_sorted()
{
  std::printf("test_cross_margin_liquidation_legs_sorted\n");
  Ledger led;
  std::vector<Liquidation> liqs;
  CrossMarginManager cm(led, QUOTE, VENUE, [&](const Liquidation& l)
                        { liqs.push_back(l); });

  std::vector<SymbolId> syms;
  for (uint64_t s : kScattered)
  {
    const auto id = static_cast<SymbolId>(s % 60000 + 1);
    cm.configureSymbol(id, /*imBps*/ 1000, /*mmBps*/ 500);
    syms.push_back(id);
  }
  std::sort(syms.begin(), syms.end());
  syms.erase(std::unique(syms.begin(), syms.end()), syms.end());

  // A wallet thinner than the loss the crash below realizes: the account goes
  // bankrupt and every leg closes in the same liquidate() call.
  led.deposit(ACCT, QUOTE, quote(1000));
  led.deposit(VENUE, QUOTE, quote(1'000'000));

  // Open a long leg on every symbol, then crash every mark at once. Pausing
  // keeps the sweep from firing between marks, so the whole portfolio goes in
  // one liquidate() call -- which is what puts every leg in one emission run.
  cm.setLiquidationsPaused(true);
  for (SymbolId s : syms)
  {
    cm.applyFill(ACCT, s, Side::BUY, qty(1).raw(), px(100).raw());
    cm.setMark(s, px(1.0));
  }
  cm.setLiquidationsPaused(false);
  cm.setMark(syms.front(), px(1.0));  // one sweep, whole portfolio

  CHECK(liqs.size() == syms.size());
  std::vector<SymbolId> got;
  for (const auto& l : liqs)
  {
    got.push_back(l.symbol);
  }
  CHECK(std::is_sorted(got.begin(), got.end()));
  CHECK(got == syms);
}

// ---------------------------------------------------------------------------
// MarketDataPublisher::snapshotAtomic -- the body a late joiner applies.
//
// MdRecoveryServer encodes this vector record by record onto the recovery
// channel, so an unsorted body makes the BYTES a recovering consumer reads
// depend on the publisher's standard library. Every consumer rebuilds the same
// book either way, which is exactly what hides it from a state comparison.
void test_md_snapshot_body_sorted()
{
  std::printf("test_md_snapshot_body_sorted\n");
  // Heap, not stack, as market_data.h says and as every other publisher in the
  // suite is held: at the default level count the object is ~2 MB of
  // preallocated ladder and a Windows thread gets a one-megabyte stack, so the
  // stack form compiles everywhere and dies on exactly one job.
  auto mdHolder = std::make_unique<MarketDataPublisher<>>([](const MdMessage&) {}, px(0.01), SYM);
  auto& md = *mdHolder;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, 0); });
  double p = 10.0;
  for (uint64_t id : kScattered)
  {
    eng.submit(bid(static_cast<OrderId>(id), p, 1.0, ACCT));
    p += 0.01;
  }

  const auto snap = md.snapshotAtomic();
  CHECK(snap.orders.size() == kScattered.size());
  std::vector<OrderId> got;
  for (const auto& m : snap.orders)
  {
    CHECK(m.type == MdType::AddOrder);
    got.push_back(m.id);
  }
  CHECK(std::is_sorted(got.begin(), got.end()));
}

// ---------------------------------------------------------------------------
// InstrumentRegistry::list -- the control plane's answer to "list".
//
// ControlApi renders this vector into the JSON array an operator reads. An
// enumeration accessor that hands out a vector hands out an order whether or
// not it means to, and this one rehashes underneath the operator.
void test_instrument_list_sorted()
{
  std::printf("test_instrument_list_sorted\n");
  InstrumentRegistry reg;
  std::vector<SymbolId> want;
  for (uint64_t s : kScattered)
  {
    const auto id = static_cast<SymbolId>(s % 60000 + 1);
    if (reg.listInstrument(cfg(id)))
    {
      want.push_back(id);
    }
  }
  std::sort(want.begin(), want.end());

  const auto got = reg.list();
  CHECK(got.size() == want.size());
  CHECK(std::is_sorted(got.begin(), got.end()));
  CHECK(got == want);
}

// ---------------------------------------------------------------------------
// SymbolRouter::snapshotAccount -- the cross-shard reconnect view.
//
// Per symbol the frames are already ordered (the engine sorts them above);
// across symbols they would go out in the order the engines happen to sit in
// the router's map.
void test_router_snapshot_symbols_sorted()
{
  std::printf("test_router_snapshot_symbols_sorted\n");
  SymbolRouter<MatchingBook> router(/*shards*/ 4);
  std::vector<SymbolId> want;
  for (uint64_t s : kScattered)
  {
    const auto id = static_cast<SymbolId>(s % 60000 + 1);
    if (router.has(id))
    {
      continue;
    }
    router.addSymbol(cfg(id), [](const OutboundEvent&) {});
    want.push_back(id);
  }
  std::sort(want.begin(), want.end());
  for (SymbolId s : want)
  {
    router.submit(bid(static_cast<OrderId>(s) + 1, 10.0, 1.0, ACCT, s));
  }

  const auto snaps = router.snapshotAccount(ACCT);
  CHECK(snaps.size() == want.size());
  std::vector<SymbolId> got;
  for (const auto& [sym, s] : snaps)
  {
    CHECK(s.openOrders.size() == 1);
    got.push_back(sym);
  }
  CHECK(std::is_sorted(got.begin(), got.end()));
  CHECK(got == want);
}

}  // namespace

TEST(IterationOrder, EngineSuite)
{
  test_engine_snapshot_open_orders_sorted();
  test_collateral_assets_sorted();
  test_collateral_conversion_picks_lowest_asset();
  test_cross_margin_liquidation_legs_sorted();
  test_md_snapshot_body_sorted();
  test_instrument_list_sorted();
  test_router_snapshot_symbols_sorted();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
