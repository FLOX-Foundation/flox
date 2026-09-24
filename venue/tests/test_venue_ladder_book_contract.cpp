/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What LadderBook promises the engine, asked of the engine rather than of the
// book's own comments.
//
// Three promises are written down and none of them is checked anywhere:
//
//   1. "O(1) best ... no steady-state allocation" (docs/venue/matching.md:14).
//      The id index is open-addressed and eraseSlot only writes a tombstone,
//      so an order lifecycle -- add, cancel -- leaves a tombstone behind and
//      nothing ever reclaims one. validate() calls contains() on every new
//      order id, so the probe chain the engine walks per order grows with the
//      number of orders the venue has ever seen, not with the number resting.
//
//   2. "capacity -- engine should gate via full() before matching" and
//      "out of band -- engine's collar must prevent this" (ladder_book.h:89,
//      :97). addResting just returns in both cases. full() is called by
//      nothing in the tree, and the collar is optional config, so an order
//      the book refused is still acked to its owner as accepted and working.
//
//   3. "Level index = (price - base) / tick" (ladder_book.h:19). The division
//      truncates toward zero, so a price in (base - tick, base) lands on
//      level 0 instead of being refused, and the ladder's price index then
//      disagrees with the price on the order sitting in it.
//
// Each test below states one of these as a number.

#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

NewOrder limit(OrderId id, Side s, double p, double q)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = 7;
  return o;
}

RestingOrder resting(OrderId id, Side s, double p, double q)
{
  return RestingOrder{.id = id, .accountId = 7, .price = px(p), .leaves = qty(q), .side = s};
}

// Every event the engine emitted, in order.
struct Tape
{
  std::vector<OutboundEvent> events;

  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { events.push_back(e); };
  }

  size_t accepts(OrderId id) const
  {
    size_t n = 0;
    for (const auto& e : events)
    {
      const auto* a = std::get_if<OrderAccepted>(&e);
      n += (a != nullptr && a->id == id) ? 1 : 0;
    }
    return n;
  }
  const OrderRejected* reject(OrderId id) const
  {
    for (const auto& e : events)
    {
      if (const auto* r = std::get_if<OrderRejected>(&e); r != nullptr && r->id == id)
      {
        return r;
      }
    }
    return nullptr;
  }
  size_t countAccepts() const
  {
    size_t n = 0;
    for (const auto& e : events)
    {
      n += std::holds_alternative<OrderAccepted>(e) ? 1 : 0;
    }
    return n;
  }
  size_t countRejects() const
  {
    size_t n = 0;
    for (const auto& e : events)
    {
      n += std::holds_alternative<OrderRejected>(e) ? 1 : 0;
    }
    return n;
  }
  const Trade* firstTrade() const
  {
    for (const auto& e : events)
    {
      if (const auto* t = std::get_if<Trade>(&e))
      {
        return t;
      }
    }
    return nullptr;
  }
};

// ---- finding 4: the cost of a lookup, measured ----------------------------

// Nanoseconds per contains() call, as the median over `batches` batches of
// `perBatch` lookups. Batched because one lookup on a healthy index is below
// the clock's resolution; the median because a sample is a wall-clock
// measurement on a shared machine and the mean carries whatever else ran.
//
// The id walks a span rather than repeating: a single invariant id would let
// the compiler hoist the call out of the loop and measure nothing. The hit
// count is checked against what the caller expects for the same reason -- an
// unused result is a loop the optimiser is free to delete.
double lookupNs(const LadderBook& book, OrderId first, uint64_t span, int batches, int perBatch,
                bool expectHits)
{
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(batches));
  uint64_t hits = 0;
  for (int b = 0; b < batches; ++b)
  {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < perBatch; ++i)
    {
      hits += book.contains(first + (static_cast<uint64_t>(i) % span)) ? 1u : 0u;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    samples.push_back(static_cast<double>(ns) / perBatch);
  }
  EXPECT_EQ(hits, expectHits ? static_cast<uint64_t>(batches) * static_cast<uint64_t>(perBatch)
                             : 0u)
      << "the lookups did not answer what the book holds";
  std::sort(samples.begin(), samples.end());
  // A floor at a quarter of a nanosecond per lookup: below that the number is
  // the clock's, not the index's, and a zero would make the ratio below
  // meaningless.
  return std::max(samples[samples.size() / 2], 0.25);
}

constexpr uint64_t kCycles = 1'000'000;  // add+cancel pairs of distinct ids
constexpr OrderId kChurnBase = 1'000'000;
constexpr OrderId kProbeBase = 100;  // the orders left resting, added last
constexpr uint64_t kProbes = 64;
constexpr OrderId kAbsentBase = 900'000'000;  // ids the book never held
constexpr uint64_t kAbsentSpan = 64;

LadderBook::Config indexCfg()
{
  return LadderBook::Config{
      .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 4096, .maxOrders = 1 << 14};
}

void restProbes(LadderBook& book)
{
  for (uint64_t i = 0; i < kProbes; ++i)
  {
    book.addResting(Side::BUY, resting(kProbeBase + i, Side::BUY, 1.0 + 0.01 * double(i), 1.0));
  }
}

}  // namespace

// A venue runs for a day; the index must not care how many orders went through
// it, only how many are in it. One million add/cancel pairs is a quiet morning
// on one instrument.
//
// Two lookups are timed because the engine does both: contains() on an id the
// book does not hold is validate()'s duplicate-id gate on every single new
// order, and contains()/find() on a resting id is cancel, modify and every
// report. Three times the fresh cost is already generous -- the promise is
// O(1).
TEST(LadderBookIdIndex, ALookupCostsTheSameAfterAMillionOrderLifecycles)
{
  LadderBook fresh(indexCfg());
  restProbes(fresh);
  const double freshAbsent = lookupNs(fresh, kAbsentBase, kAbsentSpan, 65, 2000, false);
  const double freshPresent = lookupNs(fresh, kProbeBase, kProbes, 65, 2000, true);

  LadderBook aged(indexCfg());
  for (uint64_t i = 0; i < kCycles; ++i)
  {
    const OrderId id = kChurnBase + i;
    aged.addResting(Side::BUY, resting(id, Side::BUY, 1.0, 1.0));
    ASSERT_TRUE(aged.cancel(id).has_value()) << "churn order " << id << " never reached the index";
  }
  ASSERT_TRUE(aged.empty()) << "the churn left orders behind; the measurement is not comparable";
  restProbes(aged);

  // A fast index that lost the orders would pass the timing and fail the venue.
  for (uint64_t i = 0; i < kProbes; ++i)
  {
    ASSERT_TRUE(aged.contains(kProbeBase + i))
        << "order " << (kProbeBase + i) << " rests but the id index does not have it";
  }

  const double agedAbsent = lookupNs(aged, kAbsentBase, kAbsentSpan, 65, 2000, false);
  const double agedPresent = lookupNs(aged, kProbeBase, kProbes, 65, 2000, true);

  EXPECT_LE(agedAbsent, 3.0 * freshAbsent)
      << "contains() on a new id: " << freshAbsent << " ns fresh, " << agedAbsent << " ns after "
      << kCycles << " lifecycles. validate() pays this on every order";
  EXPECT_LE(agedPresent, 3.0 * freshPresent)
      << "contains() on a resting id: " << freshPresent << " ns fresh, " << agedPresent
      << " ns after " << kCycles << " lifecycles";
}

// ---- finding 8: an order the book did not take is not accepted ------------

// Two ways the book refuses an order in silence: a price with no level, and an
// exhausted node pool. Both end with addResting returning and the engine
// sending OrderAccepted for an order that is not on the book -- the owner has
// a working order the venue does not have.
//
// The collar (SymbolConfig min/maxPrice) is optional config and defaults to
// unchecked, so it is not the answer: the book's band is a property of the
// book and has to be refused by the engine that owns the book.
TEST(LadderBookAcceptance, AnOrderTheBookCannotTakeIsRejectedNotAccepted)
{
  SymbolConfig cfg;
  cfg.id = SYM;
  cfg.tickSize = px(0.01);

  // The ladder spans [0, 1000.00); the collar is off, as it is by default.
  const LadderBook::Config ladder{
      .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 100'000, .maxOrders = 4};

  RejectReason outOfBand = RejectReason::None;
  RejectReason poolFull = RejectReason::None;

  {
    Tape tape;
    MatchingEngine<LadderBook> eng(cfg, tape.sink(), LadderBook{ladder});
    eng.submit(InboundCommand{limit(1, Side::BUY, 2000.0, 1.0)}, 1);

    EXPECT_EQ(tape.accepts(1), 0u) << "a price above the ladder's top level was acked as working";
    const OrderRejected* r = tape.reject(1);
    EXPECT_NE(r, nullptr) << "the order is on no book and its owner was told nothing";
    if (r != nullptr)
    {
      EXPECT_NE(r->reason, RejectReason::None) << "a reject must carry a reason the owner can read";
      outOfBand = r->reason;
    }
    EXPECT_FALSE(eng.book().contains(1));
    EXPECT_EQ(eng.restingOrderCount(), 0u) << "the engine is tracking an order the book refused";
  }

  {
    Tape tape;
    MatchingEngine<LadderBook> eng(cfg, tape.sink(), LadderBook{ladder});
    for (OrderId id = 1; id <= 4; ++id)
    {
      eng.submit(InboundCommand{limit(id, Side::BUY, 10.0 + double(id), 1.0)}, int64_t(id));
      ASSERT_EQ(tape.accepts(id), 1u) << "order " << id << " fits the pool and must rest";
    }
    ASSERT_TRUE(eng.book().full()) << "four orders in a pool of four";

    eng.submit(InboundCommand{limit(5, Side::BUY, 20.0, 1.0)}, 5);

    EXPECT_EQ(tape.accepts(5), 0u) << "an order the exhausted pool dropped was acked as working";
    const OrderRejected* r = tape.reject(5);
    EXPECT_NE(r, nullptr) << "the order is on no book and its owner was told nothing";
    if (r != nullptr)
    {
      EXPECT_NE(r->reason, RejectReason::None) << "a reject must carry a reason the owner can read";
      poolFull = r->reason;
    }
    EXPECT_FALSE(eng.book().contains(5));
    EXPECT_EQ(eng.restingOrderCount(), 4u) << "the engine is tracking an order the book refused";
  }

  // The two are different failures with different answers: one is the client's
  // price, the other is the venue running out of room, and a client that
  // cannot tell them apart retries the one it should not.
  // W33-T003 needs: a RejectReason value for the exhausted pool (the price
  // case is already served by InvalidPrice).
  EXPECT_NE(outOfBand, poolFull) << "out-of-band price and exhausted pool answer the same reason: "
                                 << toString(outOfBand);
}

// ---- finding 9: a price below the base -------------------------------------

// levelOf divides toward zero, so 995 with base 1000 and a tick of 10 gives
// level 0 -- the base level -- instead of the negative index that would have
// refused it. The order keeps its own price, the ladder files it under 1000,
// and the two never agree again.
//
// docs/venue/matching.md:17-22: "They are interchangeable
// (MatchingEngine<MatchingBook> / MatchingEngine<LadderBook>) ... the two are
// contractually required to agree." The map book rests the ask at 995 and
// answers bestAsk() 995, so either the ladder does the same or the engine
// refuses the order -- what it must not do is quote 995 of liquidity at 1000.
TEST(LadderBookPriceLevels, APriceBelowTheLadderBaseDoesNotLandOnTheBaseLevel)
{
  SymbolConfig cfg;
  cfg.id = SYM;
  // tickSize 0 = unchecked, the documented default: nothing upstream rounds
  // the price, so the book gets it as the client sent it.

  const LadderBook::Config ladder{.basePriceRaw = px(1000.0).raw(),
                                  .tickRaw = px(10.0).raw(),
                                  .numLevels = 100,
                                  .maxOrders = 64};

  Tape oracle;
  MatchingEngine<MatchingBook> ref(cfg, oracle.sink());
  ref.submit(InboundCommand{limit(1, Side::SELL, 995.0, 1.0)}, 1);
  ASSERT_TRUE(ref.book().bestAsk().has_value());
  const int64_t oracleBestAsk = ref.book().bestAsk().value().raw();
  ASSERT_EQ(oracleBestAsk, px(995.0).raw());

  Tape tape;
  MatchingEngine<LadderBook> eng(cfg, tape.sink(), LadderBook{ladder});
  eng.submit(InboundCommand{limit(1, Side::SELL, 995.0, 1.0)}, 1);

  const bool refused = tape.reject(1) != nullptr;
  if (!refused)
  {
    ASSERT_EQ(tape.accepts(1), 1u) << "neither accepted nor rejected";
    ASSERT_TRUE(eng.book().contains(1)) << "acked as working and on no level";
    const RestingOrder* o = eng.book().find(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->price.raw(), px(995.0).raw()) << "the order's own price was rewritten";
    ASSERT_TRUE(eng.book().bestAsk().has_value());
    EXPECT_EQ(eng.book().bestAsk().value().raw(), px(995.0).raw())
        << "the ladder files a 995 ask under a different price";
    EXPECT_EQ(eng.book().bestAsk().value().raw(), oracleBestAsk)
        << "the ladder and the reference book disagree about the same order";

    // A buyer at 1000 must not be told it met an ask that is not there.
    eng.submit(InboundCommand{limit(2, Side::BUY, 1000.0, 1.0)}, 2);
    if (const Trade* t = tape.firstTrade(); t != nullptr)
    {
      EXPECT_NE(t->price.raw(), px(1000.0).raw())
          << "a 995 ask printed at 1000: the level, not the order, set the price";
    }
  }

  EXPECT_TRUE(refused || eng.book().bestAsk().value_or(Price{}).raw() == px(995.0).raw())
      << "a price below the ladder base was neither refused nor kept at its own price";
}

// ---- finding 8 (the gate itself): full() ----------------------------------

// full() exists to be asked before matching and is called by nothing in the
// tree. The pool is what it counts, so the count is what this states: with
// room for eight, the ninth and tenth orders are refused, and a cancel makes
// room for exactly one more.
TEST(LadderBookCapacity, FullGatesTheEngineAtTheLastNode)
{
  {
    // The flag itself, on the book alone.
    LadderBook book(LadderBook::Config{
        .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 1000, .maxOrders = 3});
    EXPECT_FALSE(book.full());
    for (OrderId id = 1; id <= 3; ++id)
    {
      book.addResting(Side::BUY, resting(id, Side::BUY, 1.0 + 0.01 * double(id), 1.0));
    }
    EXPECT_TRUE(book.full()) << "three nodes of three are in use";
    ASSERT_TRUE(book.cancel(2).has_value());
    EXPECT_FALSE(book.full()) << "a cancelled order returns its node to the pool";
  }

  SymbolConfig cfg;
  cfg.id = SYM;
  cfg.tickSize = px(0.01);

  Tape tape;
  MatchingEngine<LadderBook> eng(
      cfg, tape.sink(),
      LadderBook{LadderBook::Config{
          .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 100'000, .maxOrders = 8}});

  for (OrderId id = 1; id <= 10; ++id)
  {
    eng.submit(InboundCommand{limit(id, Side::BUY, 10.0 + double(id), 1.0)}, int64_t(id));
  }

  EXPECT_EQ(tape.countAccepts(), 8u) << "the pool holds eight and the engine acked more";
  EXPECT_EQ(tape.countRejects(), 2u) << "the two orders the book dropped were never refused";
  EXPECT_TRUE(eng.book().full());
  EXPECT_EQ(eng.restingOrderCount(), 8u);

  CancelOrder c;
  c.id = 1;
  c.symbol = SYM;
  c.accountId = 7;
  eng.submit(InboundCommand{c}, 11);
  EXPECT_FALSE(eng.book().full()) << "a cancel returned no node to the pool";

  eng.submit(InboundCommand{limit(11, Side::BUY, 30.0, 1.0)}, 12);
  EXPECT_EQ(tape.accepts(11), 1u) << "the freed node was not reused";
  EXPECT_TRUE(eng.book().contains(11));
  EXPECT_EQ(eng.restingOrderCount(), 8u);
}
