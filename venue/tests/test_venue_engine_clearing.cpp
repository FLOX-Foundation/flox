/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * engine::Clearing on its own -- no MatchingEngine, no book, no matcher.
 *
 * The engine's own perp tests drive this code through order flow, which means
 * a failure there could be the matcher, the reservation path or clearing, and
 * a clearing rule nobody trades into is not covered at all. Here the component
 * is built with a ledger, a config and three hooks standing in for the engine,
 * and every rule is stated directly: who pays funding, when a position is
 * liquidated, what order the ADL victims are taken in, and which bytes the
 * snapshot carries.
 */

#include "flox-venue/engine/clearing.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr AssetId USD = 1;
constexpr uint64_t VENUE = 999;

int64_t i64(Amount a) { return static_cast<int64_t>(a); }
Amount money(double v) { return amountOf(Volume::fromDouble(v)); }
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

// The engine, reduced to what clearing actually asks of it: a reservation
// table the initial margin comes out of, and a record of whose resting orders
// were cancelled. Same arithmetic as MatchingEngine::imForRaw.
struct Host
{
  struct Reservation
  {
    Amount reservedRaw{0};
    int64_t limitPriceRaw{0};
  };

  SymbolConfig cfg;
  Ledger led;
  std::vector<OutboundEvent> out;
  EventSink sink;
  std::unordered_map<OrderId, Reservation> reserve;
  std::vector<uint64_t> cancelled;
  engine::Clearing clearing;

  explicit Host(SymbolConfig c)
      : cfg(c),
        sink([this](const OutboundEvent& e)
             { out.push_back(e); }),
        clearing(cfg, sink,
                 engine::Clearing::Hooks{
                     this,
                     [](void* ctx, OrderId id, int64_t q)
                     { return static_cast<Host*>(ctx)->consumeOrderIM(id, q); },
                     [](void* ctx, OrderId id, int64_t q, uint64_t acct)
                     { static_cast<Host*>(ctx)->releaseOrderIM(id, q, acct); },
                     [](void* ctx, uint64_t acct)
                     { static_cast<Host*>(ctx)->cancelled.push_back(acct); }})
  {
    clearing.setLedger(&led, VENUE);
  }

  Amount imFor(int64_t qtyRaw, int64_t priceRaw) const
  {
    return notionalRaw(priceRaw, qtyRaw, cfg.priceScale, cfg.qtyScale) * cfg.initialMarginBps /
           10000;
  }

  // Reserve the initial margin for an order the way the engine's reserveFunds
  // does, so the fill below has something to consume.
  void placeOrder(OrderId id, uint64_t acct, int64_t qtyRaw, int64_t priceRaw)
  {
    const Amount im = imFor(qtyRaw, priceRaw);
    EXPECT_TRUE(led.reserve(acct, USD, im));
    reserve[id] = Reservation{im, priceRaw};
  }

  Amount consumeOrderIM(OrderId id, int64_t qtyRaw)
  {
    auto it = reserve.find(id);
    if (it == reserve.end())
    {
      return 0;
    }
    Amount im = imFor(qtyRaw, it->second.limitPriceRaw);
    if (im > it->second.reservedRaw)
    {
      im = it->second.reservedRaw;
    }
    it->second.reservedRaw -= im;
    return im;
  }

  void releaseOrderIM(OrderId id, int64_t qtyRaw, uint64_t acct)
  {
    auto it = reserve.find(id);
    if (it == reserve.end())
    {
      return;
    }
    Amount im = imFor(qtyRaw, it->second.limitPriceRaw);
    if (im > it->second.reservedRaw)
    {
      im = it->second.reservedRaw;
    }
    it->second.reservedRaw -= im;
    if (im > 0)
    {
      led.release(acct, USD, im);
    }
  }

  int liquidations() const
  {
    int n = 0;
    for (const auto& e : out)
    {
      n += std::holds_alternative<Liquidation>(e) ? 1 : 0;
    }
    return n;
  }

  std::vector<Liquidation> liqs() const
  {
    std::vector<Liquidation> v;
    for (const auto& e : out)
    {
      if (const auto* l = std::get_if<Liquidation>(&e))
      {
        v.push_back(*l);
      }
    }
    return v;
  }
};

SymbolConfig perpCfg()
{
  SymbolConfig c;
  c.id = 7;
  c.linearPerp = true;
  c.quoteAsset = USD;
  c.initialMarginBps = 1000;     // 10x
  c.maintenanceMarginBps = 500;  // 5%
  return c;
}

// A snapshot journal in a scratch file, read back as the records the venue
// would replay.
std::vector<std::pair<int64_t, InboundCommand>> roundTrip(
    const std::function<void(Journal&)>& write)
{
  const std::string path =
      (std::filesystem::temp_directory_path() /
       ("flox_clearing_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
        std::to_string(reinterpret_cast<uintptr_t>(&write)) + ".jrn"))
          .string();
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    write(j);
    j.flush();
  }
  auto records = Journal::loadTimed(path);
  std::remove(path.c_str());
  return records;
}

}  // namespace

// ---- positions -----------------------------------------------------------

TEST(EngineClearing, OpeningFillTakesMarginFromTheOrderReservation)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(10000));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());  // notional 1000, IM 100
  EXPECT_EQ(i64(h.led.reserved(1, USD)), i64(money(100)));

  h.clearing.updatePerpPosition(1, 100, /*fillBuy*/ true, qty(2).raw(), px(500).raw());

  EXPECT_EQ(h.clearing.positionQty(1), qty(2).raw());
  EXPECT_EQ(h.clearing.positionEntry(1).raw(), px(500).raw());
  // The margin moved from the order to the position; the ledger still holds it
  // reserved, so the account's available is unchanged.
  EXPECT_EQ(i64(h.clearing.totalPositionMargin()), i64(money(100)));
  EXPECT_EQ(i64(h.led.reserved(1, USD)), i64(money(100)));
  EXPECT_EQ(i64(h.led.available(1, USD)), i64(money(9900)));
}

TEST(EngineClearing, AverageEntryIsQuantityWeighted)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(10000));
  h.placeOrder(100, 1, qty(1).raw(), px(100).raw());
  h.placeOrder(101, 1, qty(3).raw(), px(200).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(1).raw(), px(100).raw());
  h.clearing.updatePerpPosition(1, 101, true, qty(3).raw(), px(200).raw());

  EXPECT_EQ(h.clearing.positionQty(1), qty(4).raw());
  EXPECT_EQ(h.clearing.positionEntry(1).raw(), px(175).raw());  // (1*100 + 3*200) / 4
}

TEST(EngineClearing, ReducingFillRealizesPnlThroughTheClearingPool)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(10000));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());

  h.placeOrder(101, 1, qty(2).raw(), px(600).raw());
  h.clearing.updatePerpPosition(1, 101, /*fillBuy*/ false, qty(2).raw(), px(600).raw());

  // Flat, margin returned, +200 realized, and the venue paid exactly that.
  EXPECT_EQ(h.clearing.positionQty(1), 0);
  EXPECT_EQ(i64(h.clearing.totalPositionMargin()), 0);
  EXPECT_EQ(i64(h.led.reserved(1, USD)), 0);
  EXPECT_EQ(i64(h.led.available(1, USD)), i64(money(10200)));
  EXPECT_EQ(i64(h.led.available(VENUE, USD)), i64(money(-200)));
}

TEST(EngineClearing, OpenInterestIsTheLongSideAndUnrealizedFollowsTheSide)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(10000));
  h.led.deposit(2, USD, money(10000));
  h.placeOrder(100, 1, qty(3).raw(), px(500).raw());
  h.placeOrder(200, 2, qty(3).raw(), px(500).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(3).raw(), px(500).raw());
  h.clearing.updatePerpPosition(2, 200, false, qty(3).raw(), px(500).raw());

  EXPECT_EQ(h.clearing.openInterest().raw(), qty(3).raw());  // not 6, not 0
  EXPECT_EQ(i64(h.clearing.unrealizedPnlRaw(1, px(510))), i64(money(30)));
  EXPECT_EQ(i64(h.clearing.unrealizedPnlRaw(2, px(510))), i64(money(-30)));
  EXPECT_EQ(i64(h.clearing.unrealizedPnlRaw(3, px(510))), 0);  // no position
}

TEST(EngineClearing, AdjustPositionCorrectsWithoutMovingMoney)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(1000));

  AdjustPosition empty{};
  empty.accountId = 1;
  h.clearing.adjustPosition(empty);
  ASSERT_EQ(h.out.size(), 1u);
  const auto* rej = std::get_if<OrderRejected>(&h.out[0]);
  ASSERT_NE(rej, nullptr);
  EXPECT_EQ(rej->reason, RejectReason::AdjustmentEmpty);

  AdjustPosition noEntry{};
  noEntry.accountId = 1;
  noEntry.qtyDeltaRaw = qty(1).raw();
  h.clearing.adjustPosition(noEntry);
  ASSERT_EQ(h.out.size(), 2u);
  EXPECT_EQ(std::get<OrderRejected>(h.out[1]).reason, RejectReason::AdjustmentNeedsEntry);

  AdjustPosition ok = noEntry;
  ok.entryRaw = px(50).raw();
  h.clearing.adjustPosition(ok);
  ASSERT_EQ(h.out.size(), 3u);
  const auto* adj = std::get_if<PositionAdjusted>(&h.out[2]);
  ASSERT_NE(adj, nullptr);
  EXPECT_EQ(adj->qtyAfterRaw, qty(1).raw());
  EXPECT_EQ(adj->entryAfterRaw, px(50).raw());
  EXPECT_EQ(i64(h.led.available(1, USD)), i64(money(1000)));  // no money moved

  AdjustPosition flat{};
  flat.accountId = 1;
  flat.qtyDeltaRaw = -qty(1).raw();
  h.clearing.adjustPosition(flat);
  EXPECT_EQ(h.clearing.positionQty(1), 0);
  EXPECT_EQ(std::get<PositionAdjusted>(h.out.back()).entryAfterRaw, 0);  // flat is flat
}

// ---- funding -------------------------------------------------------------

TEST(EngineClearing, LongPaysWhenTheRateIsPositive)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(10000));
  h.led.deposit(2, USD, money(10000));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());
  h.placeOrder(200, 2, qty(2).raw(), px(500).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());   // long
  h.clearing.updatePerpPosition(2, 200, false, qty(2).raw(), px(500).raw());  // short

  const Amount longBefore = h.led.available(1, USD);
  const Amount shortBefore = h.led.available(2, USD);
  h.clearing.applyFunding(0.01, px(500), SeqNanos{});  // notional 1000 -> 10 each way

  EXPECT_EQ(i64(h.led.available(1, USD)), i64(longBefore - money(10)));
  EXPECT_EQ(i64(h.led.available(2, USD)), i64(shortBefore + money(10)));
  EXPECT_EQ(i64(h.led.available(VENUE, USD)), 0);  // the pool nets to zero
  EXPECT_EQ(h.clearing.fundingRateRaw(),
            static_cast<int64_t>(0.01 * static_cast<double>(kFundingRateScale)));
}

TEST(EngineClearing, NegativeRateReversesWhoPays)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(10000));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());

  const Amount before = h.led.available(1, USD);
  h.clearing.applyFunding(-0.01, px(500), SeqNanos{});
  EXPECT_EQ(i64(h.led.available(1, USD)), i64(before + money(10)));  // the long is paid
}

TEST(EngineClearing, FundingWithNoLedgerStillRecordsTheRate)
{
  Host h(perpCfg());
  h.clearing.setLedger(nullptr, VENUE);
  h.clearing.applyFunding(0.02, px(500), SeqNanos{});
  EXPECT_EQ(h.clearing.fundingRateRaw(),
            static_cast<int64_t>(0.02 * static_cast<double>(kFundingRateScale)));
}

TEST(EngineClearing, ScheduleOverridesTheConfigDerivationAndStepsOnSettlement)
{
  SymbolConfig c = perpCfg();
  c.fundingIntervalNs = DurationNs{1000};
  Host h(c);

  // No schedule set: the boundary is derived from (now, interval).
  EXPECT_EQ(h.clearing.nextFundingNs(SeqNanos::fromRaw(2500)).raw(), 3000);
  EXPECT_EQ(h.clearing.fundingIntervalNs(), 1000);

  h.clearing.setFundingSchedule(DurationNs{400}, SeqNanos::fromRaw(9000));
  EXPECT_EQ(h.clearing.nextFundingNs(SeqNanos::fromRaw(2500)).raw(), 9000);  // the fact wins
  EXPECT_EQ(h.clearing.fundingIntervalNs(), 400);

  // One settlement steps one interval.
  h.clearing.applyFunding(0.0, px(500), SeqNanos::fromRaw(9000));
  EXPECT_EQ(h.clearing.nextFundingNs(SeqNanos::fromRaw(9000)).raw(), 9400);

  // A settlement that ran late skips whole intervals until the boundary is
  // ahead of `now` again.
  h.clearing.applyFunding(0.0, px(500), SeqNanos::fromRaw(11000));
  // 9400 -> 9800 is still behind 11000, so it skips whole intervals: 9800 +
  // (1200 / 400 + 1) * 400 = 11400.
  EXPECT_GT(h.clearing.nextFundingNs(SeqNanos::fromRaw(11000)).raw(), 11000);
  EXPECT_EQ(h.clearing.nextFundingNs(SeqNanos::fromRaw(11000)).raw(), 11400);

  // Clearing the schedule drops back to the derivation.
  h.clearing.setFundingSchedule(DurationNs{}, SeqNanos{});
  EXPECT_EQ(h.clearing.nextFundingNs(SeqNanos::fromRaw(2500)).raw(), 3000);
}

// ---- liquidation ---------------------------------------------------------

TEST(EngineClearing, LiquidatesOnlyOnceEquityIsBelowMaintenance)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(10000));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());  // IM 100 on notional 1000
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());

  // At 460: uPnl = -80, equity = 20; mmReq = 5% of 920 = 46 -> below, liquidate.
  // At 480: uPnl = -40, equity = 60; mmReq = 5% of 960 = 48 -> above, hold.
  h.clearing.checkLiquidations(px(480));
  EXPECT_EQ(h.liquidations(), 0);
  EXPECT_EQ(h.clearing.positionQty(1), qty(2).raw());

  h.clearing.checkLiquidations(px(460));
  ASSERT_EQ(h.liquidations(), 1);
  EXPECT_EQ(h.clearing.positionQty(1), 0);
  EXPECT_FALSE(h.liqs()[0].bankrupt);
  // The account's resting orders went first.
  ASSERT_EQ(h.cancelled.size(), 1u);
  EXPECT_EQ(h.cancelled[0], 1u);
}

TEST(EngineClearing, ExternalLiquidationOwnerSuppressesTheSweep)
{
  SymbolConfig c = perpCfg();
  c.externalLiquidation = true;
  Host h(c);
  h.led.deposit(1, USD, money(10000));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());

  h.clearing.checkLiquidations(px(400));
  EXPECT_EQ(h.liquidations(), 0);
  EXPECT_EQ(h.clearing.positionQty(1), qty(2).raw());

  // But the owner's own force-close still works, through the same path.
  h.clearing.forceClose(1, px(400));
  EXPECT_EQ(h.liquidations(), 1);
  EXPECT_EQ(h.clearing.positionQty(1), 0);
}

TEST(EngineClearing, BreachingAccountsAreClosedInAccountIdOrder)
{
  Host h(perpCfg());
  // Inserted in an order that is not ascending, so a bucket-order traversal
  // would publish them in some other sequence.
  for (uint64_t acct : {70u, 3u, 41u, 12u})
  {
    h.led.deposit(acct, USD, money(10000));
    h.placeOrder(static_cast<OrderId>(acct), acct, qty(2).raw(), px(500).raw());
    h.clearing.updatePerpPosition(acct, static_cast<OrderId>(acct), true, qty(2).raw(),
                                  px(500).raw());
  }
  h.clearing.checkLiquidations(px(460));

  const auto ls = h.liqs();
  ASSERT_EQ(ls.size(), 4u);
  EXPECT_EQ(ls[0].account, 3u);
  EXPECT_EQ(ls[1].account, 12u);
  EXPECT_EQ(ls[2].account, 41u);
  EXPECT_EQ(ls[3].account, 70u);
}

TEST(EngineClearing, BankruptcyIsToppedUpByTheInsuranceFund)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(100));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());  // IM 100: all of it
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());
  EXPECT_EQ(i64(h.led.available(1, USD)), 0);

  h.clearing.forceClose(1, px(400));  // -200 against 100 of margin

  const auto ls = h.liqs();
  ASSERT_EQ(ls.size(), 1u);
  EXPECT_TRUE(ls[0].bankrupt);
  EXPECT_EQ(i64(h.led.available(1, USD)), 0);                    // topped up to zero, never negative
  EXPECT_EQ(i64(h.led.available(VENUE, USD)), i64(money(100)));  // +200 PnL, -100 deficit
}

TEST(EngineClearing, AutoDeleverageTakesTheMostProfitableOppositeSideFirst)
{
  SymbolConfig c = perpCfg();
  c.autoDeleverage = true;
  Host h(c);

  // The bankrupt long: 4 contracts at 500 backed by exactly its 200 of IM.
  // Closed at 300 it is 800 down, so after the margin comes back the deficit
  // is 600 -- more than the first ADL victim's whole gain, so the waterfall
  // has to reach the second one and the ORDER is observable.
  h.led.deposit(1, USD, money(200));
  h.placeOrder(100, 1, qty(4).raw(), px(500).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(4).raw(), px(500).raw());

  // Two winning shorts. 3 entered higher, so at mark 300 it is the more
  // profitable one (+300 against +200) and must be deleveraged first.
  h.led.deposit(2, USD, money(10000));
  h.placeOrder(200, 2, qty(1).raw(), px(500).raw());
  h.clearing.updatePerpPosition(2, 200, false, qty(1).raw(), px(500).raw());
  h.led.deposit(3, USD, money(10000));
  h.placeOrder(300, 3, qty(1).raw(), px(600).raw());
  h.clearing.updatePerpPosition(3, 300, false, qty(1).raw(), px(600).raw());

  h.clearing.forceClose(1, px(300));

  const auto ls = h.liqs();
  ASSERT_EQ(ls.size(), 3u);
  EXPECT_EQ(ls[0].account, 1u);
  EXPECT_TRUE(ls[0].bankrupt);
  EXPECT_EQ(ls[1].account, 3u);  // +300 at mark, ahead of account 2's +200
  EXPECT_TRUE(ls[1].adl);
  EXPECT_EQ(ls[2].account, 2u);
  EXPECT_TRUE(ls[2].adl);
  EXPECT_EQ(h.clearing.openInterest().raw(), 0);
  // The waterfall took the whole of 3's gain and the whole of 2's: both are
  // back at exactly what they deposited, and nothing was created or destroyed
  // along the way.
  EXPECT_EQ(i64(h.led.total(3, USD)), i64(money(10000)));
  EXPECT_EQ(i64(h.led.total(2, USD)), i64(money(10000)));
  EXPECT_EQ(i64(h.led.total(1, USD)), 0);
  EXPECT_EQ(i64(h.led.total(1, USD) + h.led.total(2, USD) + h.led.total(3, USD) +
                h.led.total(VENUE, USD)),
            i64(money(20200)));
}

TEST(EngineClearing, NoAutoDeleverageWhenTheConfigDoesNotAskForIt)
{
  Host h(perpCfg());  // autoDeleverage stays false
  h.led.deposit(1, USD, money(100));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());
  h.led.deposit(2, USD, money(10000));
  h.placeOrder(200, 2, qty(2).raw(), px(500).raw());
  h.clearing.updatePerpPosition(2, 200, false, qty(2).raw(), px(500).raw());

  h.clearing.forceClose(1, px(400));
  ASSERT_EQ(h.liquidations(), 1);  // the winner keeps its gain; insurance pays
  EXPECT_EQ(h.clearing.positionQty(2), -qty(2).raw());
}

TEST(EngineClearing, UnaffordableFundingDragsTheWalletIntoLiquidation)
{
  Host h(perpCfg());
  h.led.deposit(1, USD, money(100));
  h.placeOrder(100, 1, qty(2).raw(), px(500).raw());  // IM 100: nothing free left
  h.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());
  EXPECT_EQ(i64(h.led.available(1, USD)), 0);

  // Equity alone (margin 100, uPnl 0) clears the 50 maintenance requirement;
  // the wallet the funding drove negative is what tips it over.
  h.clearing.applyFunding(0.06, px(500), SeqNanos{});  // -60 on notional 1000
  EXPECT_EQ(h.liquidations(), 1);
}

// ---- checkpoint ----------------------------------------------------------

TEST(EngineClearing, PositionsAreWrittenInAccountIdOrder)
{
  Host h(perpCfg());
  for (uint64_t acct : {70u, 3u, 41u, 12u})
  {
    h.led.deposit(acct, USD, money(10000));
    h.placeOrder(static_cast<OrderId>(acct), acct, qty(1).raw(), px(500).raw());
    h.clearing.updatePerpPosition(acct, static_cast<OrderId>(acct), true, qty(1).raw(),
                                  px(500).raw());
  }

  const auto records = roundTrip([&](Journal& j)
                                 { h.clearing.writePositions(j, 1); });
  ASSERT_EQ(records.size(), 4u);
  std::vector<uint64_t> got;
  for (const auto& [ts, cmd] : records)
  {
    const auto* r = std::get_if<RestorePosition>(&cmd);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(ts, 1);
    got.push_back(r->account);
  }
  EXPECT_EQ(got, (std::vector<uint64_t>{3, 12, 41, 70}));
}

TEST(EngineClearing, FundingRecordIsWrittenOnlyWhenThereIsFundingState)
{
  Host h(perpCfg());
  EXPECT_TRUE(roundTrip([&](Journal& j)
                        { h.clearing.writeFunding(j, 1); })
                  .empty());

  h.clearing.setFundingSchedule(DurationNs{400}, SeqNanos::fromRaw(9000));
  const auto records = roundTrip([&](Journal& j)
                                 { h.clearing.writeFunding(j, 1); });
  ASSERT_EQ(records.size(), 1u);
  const auto* r = std::get_if<RestoreFunding>(&records[0].second);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->fundingIntervalNs.count(), 400);
  EXPECT_EQ(r->nextFundingNs.raw(), 9000);
}

TEST(EngineClearing, SnapshotRoundTripReproducesTheState)
{
  Host src(perpCfg());
  for (uint64_t acct : {5u, 2u})
  {
    src.led.deposit(acct, USD, money(10000));
    src.placeOrder(static_cast<OrderId>(acct), acct, qty(2).raw(), px(500).raw());
    src.clearing.updatePerpPosition(acct, static_cast<OrderId>(acct), true, qty(2).raw(),
                                    px(500).raw());
  }
  src.clearing.setFundingSchedule(DurationNs{400}, SeqNanos::fromRaw(9000));
  src.clearing.applyFunding(0.001, px(500), SeqNanos::fromRaw(9000));

  const auto records = roundTrip(
      [&](Journal& j)
      {
        src.clearing.writeFunding(j, 1);
        src.clearing.writePositions(j, 1);
      });

  Host dst(perpCfg());
  for (const auto& [ts, cmd] : records)
  {
    (void)ts;
    if (const auto* f = std::get_if<RestoreFunding>(&cmd))
    {
      dst.clearing.restoreFunding(*f);
    }
    else if (const auto* p = std::get_if<RestorePosition>(&cmd))
    {
      dst.led.deposit(p->account, USD, p->marginRaw);  // the balances arrive separately
      EXPECT_TRUE(dst.clearing.restorePosition(*p, /*exactBalanceRestore*/ false));
    }
  }

  EXPECT_EQ(dst.clearing.hashPositions(dst.clearing.hashFunding(0)),
            src.clearing.hashPositions(src.clearing.hashFunding(0)));
  EXPECT_EQ(dst.clearing.fundingRateRaw(), src.clearing.fundingRateRaw());
  EXPECT_EQ(dst.clearing.nextFundingNs(SeqNanos{}).raw(),
            src.clearing.nextFundingNs(SeqNanos{}).raw());
}

TEST(EngineClearing, RestorePositionRefusesAZeroOrDuplicateRecord)
{
  Host h(perpCfg());
  EXPECT_FALSE(h.clearing.restorePosition(RestorePosition{1, 0, px(500).raw(), 0}, true));

  h.led.deposit(1, USD, money(1000));
  EXPECT_TRUE(
      h.clearing.restorePosition(RestorePosition{1, qty(2).raw(), px(500).raw(), money(100)}, false));
  EXPECT_EQ(i64(h.led.reserved(1, USD)), i64(money(100)));
  EXPECT_FALSE(
      h.clearing.restorePosition(RestorePosition{1, qty(3).raw(), px(500).raw(), money(100)}, false));
  EXPECT_EQ(h.clearing.positionQty(1), qty(2).raw());

  // exactBalanceRestore: the margin is already split into `reserved` by the
  // balance records, so the position must not reserve it a second time.
  Host h2(perpCfg());
  h2.led.deposit(1, USD, money(1000));
  EXPECT_TRUE(h2.clearing.restorePosition(
      RestorePosition{1, qty(2).raw(), px(500).raw(), money(100)}, true));
  EXPECT_EQ(i64(h2.led.reserved(1, USD)), 0);
}

TEST(EngineClearing, TheHashSeesEveryFieldAndNotTheInsertionOrder)
{
  Host a(perpCfg());
  Host b(perpCfg());
  for (uint64_t acct : {1u, 2u, 3u})
  {
    a.led.deposit(acct, USD, money(10000));
    b.led.deposit(acct, USD, money(10000));
  }
  const std::vector<uint64_t> fwd{1, 2, 3};
  const std::vector<uint64_t> rev{3, 2, 1};
  for (uint64_t acct : fwd)
  {
    a.clearing.restorePosition(RestorePosition{acct, qty(2).raw(), px(500).raw(), money(100)}, true);
  }
  for (uint64_t acct : rev)
  {
    b.clearing.restorePosition(RestorePosition{acct, qty(2).raw(), px(500).raw(), money(100)}, true);
  }
  EXPECT_EQ(a.clearing.hashPositions(0), b.clearing.hashPositions(0));

  Host c(perpCfg());
  c.led.deposit(1, USD, money(10000));
  c.clearing.restorePosition(RestorePosition{1, qty(2).raw(), px(500).raw(), money(100)}, true);
  const uint64_t base = c.clearing.hashPositions(0);

  Host d(perpCfg());
  d.led.deposit(1, USD, money(10000));
  d.clearing.restorePosition(RestorePosition{1, qty(2).raw(), px(500).raw(), money(101)}, true);
  EXPECT_NE(d.clearing.hashPositions(0), base);  // margin is in the hash

  Host e(perpCfg());
  e.led.deposit(1, USD, money(10000));
  e.clearing.restorePosition(RestorePosition{1, qty(2).raw(), px(501).raw(), money(100)}, true);
  EXPECT_NE(e.clearing.hashPositions(0), base);  // entry is in the hash

  // An engine that never saw funding hashes as it did before the fields
  // existed; one that did, does not.
  Host f(perpCfg());
  EXPECT_EQ(f.clearing.hashFunding(0xABCDULL), 0xABCDULL);
  f.clearing.setFundingSchedule(DurationNs{400}, SeqNanos::fromRaw(9000));
  EXPECT_NE(f.clearing.hashFunding(0xABCDULL), 0xABCDULL);
}

TEST(EngineClearing, CopyStateFromCarriesPositionsAndTheCalendarOnly)
{
  Host src(perpCfg());
  src.led.deposit(1, USD, money(10000));
  src.placeOrder(100, 1, qty(2).raw(), px(500).raw());
  src.clearing.updatePerpPosition(1, 100, true, qty(2).raw(), px(500).raw());
  src.clearing.setFundingSchedule(DurationNs{400}, SeqNanos::fromRaw(9000));

  Host clone(perpCfg());
  clone.clearing.copyStateFrom(src.clearing);

  EXPECT_EQ(clone.clearing.hashPositions(clone.clearing.hashFunding(0)),
            src.clearing.hashPositions(src.clearing.hashFunding(0)));
  // The clone's wiring stayed the clone's: its own sink saw nothing, and its
  // own ledger is the one it was given.
  EXPECT_TRUE(clone.out.empty());
  EXPECT_EQ(clone.clearing.ledger(), &clone.led);
}
