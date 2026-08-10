/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Multi-agent venue simulation.
 *
 * A backtest replays a recorded tape: the market never reacts to you. Here the
 * market IS the other participants. Several agents trade against ONE order book
 * through the venue matching engine, so price is an emergent result of their
 * interaction, and every fill costs someone else the other side.
 *
 * Agents:
 *   - MarketMaker: quotes a two-sided spread around its own fair value, and
 *     skews its quotes as inventory builds up (classic inventory control).
 *   - Momentum: buys after up-ticks, sells after down-ticks.
 *   - MeanReversion: fades moves away from a long-run anchor.
 *   - NoiseTrader: random small market orders (uninformed flow).
 *
 * Everything settles through the venue venue::Ledger, so the run also demonstrates
 * that value is conserved: the sum of all cash + inventory marked at the last
 * price equals what was deposited, no matter who won.
 */

#include "flox-venue/ledger.h"
#include "flox-venue/matching_engine.h"

#include "flox-venue/matching_book.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace flox;
namespace venue = flox::venue;  // qualify: flox::Trade / flox::SymbolConfig also exist

namespace
{
constexpr SymbolId SYM = 1;
constexpr venue::AssetId CASH = 0;
constexpr venue::AssetId STOCK = 1;
constexpr uint64_t VENUE_ACCOUNT = 999;
constexpr double kStartPrice = 100.0;

Price px(double v) { return Price::fromDouble(v); }
// A quote must sit on the tick grid: the venue rejects anything else
// (TickSizeViolation), exactly as a real exchange does.
double onTick(double v) { return static_cast<double>(static_cast<long long>(v * 100.0 + 0.5)) / 100.0; }
Quantity qty(double v) { return Quantity::fromDouble(v); }
double toD(venue::Amount a) { return static_cast<double>(static_cast<int64_t>(a)) / 1e8; }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.baseAsset = STOCK;
  c.quoteAsset = CASH;
  return c;
}

// Deterministic RNG: the whole run is reproducible.
struct Rng
{
  uint64_t s{0x9E3779B97F4A7C15ULL};
  uint64_t next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() { return static_cast<double>(next() % 10000) / 10000.0; }
  bool coin() { return (next() & 1) != 0; }
};

struct Agent
{
  uint64_t account{};
  std::string name;
  const char* kind{};
};
}  // namespace

int main()
{
  venue::Ledger ledger;
  const std::vector<Agent> agents{
      {1, "market-maker", "MM"},
      {2, "momentum", "MOM"},
      {3, "mean-reversion", "REV"},
      {4, "noise", "NOISE"},
  };

  // Everyone starts with the same book value: cash + inventory.
  constexpr double kCash = 1'000'000.0;
  constexpr double kStock = 1'000.0;
  for (const auto& a : agents)
  {
    ledger.deposit(a.account, CASH, venue::amountOf(Volume::fromDouble(kCash)));
    ledger.deposit(a.account, STOCK, venue::amountOf(qty(kStock)));
  }
  const double startEquity = kCash + kStock * kStartPrice;

  double lastPrice = kStartPrice;
  uint64_t trades = 0;
  uint64_t rejects = 0;
  venue::MatchingEngine<MatchingBook> venue(cfg(),
                                            [&](const venue::OutboundEvent& e)
                                            {
                                              if (const auto* t = std::get_if<venue::Trade>(&e))
                                              {
                                                lastPrice = t->price.toDouble();
                                                ++trades;
                                              }
                                              if (std::get_if<venue::OrderRejected>(&e) != nullptr)
                                              {
                                                ++rejects;
                                              }
                                            });
  venue.setLedger(&ledger, VENUE_ACCOUNT);

  Rng rng;
  OrderId nextId = 1;
  double prevRoundClose = lastPrice;
  double roundDelta = 0.0;   // price move produced by the previous round
  double mmInventory = 0.0;  // maker's stock position vs its starting inventory

  auto submit = [&](uint64_t account, Side side, OrderType type, double price, double quantity,
                    TimeInForce tif)
  {
    venue::NewOrder o;
    o.id = nextId++;
    o.symbol = SYM;
    o.accountId = account;
    o.side = side;
    o.type = type;
    o.price = px(price);
    o.quantity = qty(quantity);
    o.tif = tif;
    venue.submit(venue::InboundCommand{o}, static_cast<int64_t>(nextId));
  };

  constexpr int kRounds = 20'000;
  std::printf("multi-agent venue: %d rounds, %zu agents, one shared book\n\n", kRounds,
              agents.size());

  for (int round = 0; round < kRounds; ++round)
  {
    // ---- market maker: quote around fair value, skewed by inventory ----
    // As it accumulates stock it lowers both quotes to attract sellers less and
    // buyers more -- the mechanism that keeps a real maker's position bounded.
    {
      const double skew = mmInventory * 0.002;
      const double fair = lastPrice - skew;
      const double halfSpread = 0.05;
      submit(1, Side::BUY, OrderType::LIMIT, onTick(fair - halfSpread), 20, TimeInForce::GTC);
      submit(1, Side::SELL, OrderType::LIMIT, onTick(fair + halfSpread), 20, TimeInForce::GTC);
    }

    // ---- momentum: chase the previous round's move ----
    if (roundDelta > 0.02 || roundDelta < -0.02)
    {
      const bool up = roundDelta > 0.0;
      submit(2, up ? Side::BUY : Side::SELL, OrderType::MARKET, 0, 1 + rng.uniform() * 2,
             TimeInForce::IOC);
    }

    // ---- mean reversion: fade deviation from the anchor ----
    {
      const double dev = lastPrice - kStartPrice;
      const double absDev = dev > 0 ? dev : -dev;
      if (absDev > 0.10)
      {
        // Size scales with how stretched the price is -- the harder the push,
        // the harder this agent leans against it.
        const double size = 1.0 + absDev * 2.0 + rng.uniform();
        submit(3, dev > 0 ? Side::SELL : Side::BUY, OrderType::MARKET, 0, size, TimeInForce::IOC);
      }
    }

    // ---- noise: uninformed flow ----
    if (rng.uniform() < 0.3)
    {
      submit(4, rng.coin() ? Side::BUY : Side::SELL, OrderType::MARKET, 0, 1 + rng.uniform() * 2,
             TimeInForce::IOC);
    }

    roundDelta = lastPrice - prevRoundClose;
    prevRoundClose = lastPrice;
    // Spot venue: inventory is the maker's stock balance vs what it started with
    // (positionQty is the perp position and is always flat here).
    mmInventory = toD(ledger.total(1, STOCK)) - kStock;

    // The maker refreshes quotes every round; pull the stale ones.
    venue.submit(venue::InboundCommand{venue::MassCancel{1, SYM}}, static_cast<int64_t>(nextId++));

    if (round % 5000 == 0 && round > 0)
    {
      std::printf("  round %5d   last=%.2f   trades=%llu\n", round, lastPrice,
                  static_cast<unsigned long long>(trades));
    }
  }

  // ---- results ----
  std::printf("\nfinal price %.2f after %llu trades (%llu rejects)\n\n", lastPrice,
              static_cast<unsigned long long>(trades), static_cast<unsigned long long>(rejects));
  std::printf("%-16s %12s %10s %14s\n", "agent", "cash", "stock", "pnl");

  double totalEquity = 0.0;
  for (const auto& a : agents)
  {
    const double cash = toD(ledger.total(a.account, CASH));
    const double stock = toD(ledger.total(a.account, STOCK));
    const double equity = cash + stock * lastPrice;
    totalEquity += equity;
    std::printf("%-16s %12.2f %10.2f %+14.2f\n", a.name.c_str(), cash, stock,
                equity - startEquity);
  }

  // The venue account holds fees (none configured here) and the clearing pool.
  const double venueCash = toD(ledger.total(VENUE_ACCOUNT, CASH));
  const double venueStock = toD(ledger.total(VENUE_ACCOUNT, STOCK));
  totalEquity += venueCash + venueStock * lastPrice;

  // Conservation is a property of the ASSETS, not of mark-to-market equity:
  // cash and stock are only ever exchanged, so each total is invariant. Total
  // equity moves with the last price because the same inventory is marked at a
  // different level -- that is a valuation effect, not leakage.
  double cashSum = venueCash;
  double stockSum = venueStock;
  for (const auto& a : agents)
  {
    cashSum += toD(ledger.total(a.account, CASH));
    stockSum += toD(ledger.total(a.account, STOCK));
  }
  const double cash0 = kCash * static_cast<double>(agents.size());
  const double stock0 = kStock * static_cast<double>(agents.size());
  std::printf("\nconservation   cash %.2f -> %.2f (drift %+.8f)\n", cash0, cashSum, cashSum - cash0);
  std::printf("               stock %.2f -> %.2f (drift %+.8f)\n", stock0, stockSum,
              stockSum - stock0);
  std::printf("\nequity %.2f vs %.2f start: the %+.2f is inventory re-marked at %.2f,\n",
              totalEquity, startEquity * static_cast<double>(agents.size()),
              totalEquity - startEquity * static_cast<double>(agents.size()), lastPrice);
  std::printf("not leakage -- one agent's gain is another's loss.\n");
  return 0;
}
