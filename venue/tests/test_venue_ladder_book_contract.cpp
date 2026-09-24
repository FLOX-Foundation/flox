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
  const OrderCanceled* canceled(OrderId id) const
  {
    for (const auto& e : events)
    {
      if (const auto* c = std::get_if<OrderCanceled>(&e); c != nullptr && c->id == id)
      {
        return c;
      }
    }
    return nullptr;
  }
  const CancelRejected* cancelRejected(OrderId id) const
  {
    for (const auto& e : events)
    {
      if (const auto* c = std::get_if<CancelRejected>(&e); c != nullptr && c->id == id)
      {
        return c;
      }
    }
    return nullptr;
  }
  const OrderModified* modified(OrderId id) const
  {
    for (const auto& e : events)
    {
      if (const auto* m = std::get_if<OrderModified>(&e); m != nullptr && m->id == id)
      {
        return m;
      }
    }
    return nullptr;
  }
  const FillHeld* held() const
  {
    for (const auto& e : events)
    {
      if (const auto* h = std::get_if<FillHeld>(&e))
      {
        return h;
      }
    }
    return nullptr;
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
    ASSERT_EQ(book.addResting(Side::BUY, resting(kProbeBase + i, Side::BUY, 1.0 + 0.01 * double(i), 1.0)),
              BookAddResult::Accepted);
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
    ASSERT_EQ(aged.addResting(Side::BUY, resting(id, Side::BUY, 1.0, 1.0)),
              BookAddResult::Accepted)
        << "the book refused churn order " << id;
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
      EXPECT_EQ(r->reason, RejectReason::InvalidPrice)
          << "a price the ladder has no level for is the client's price, like the collar's";
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
      EXPECT_EQ(r->reason, RejectReason::BookCapacityExceeded)
          << "an exhausted pool is a venue limit, not something the client can price its way out of";
      poolFull = r->reason;
    }
    EXPECT_FALSE(eng.book().contains(5));
    EXPECT_EQ(eng.restingOrderCount(), 4u) << "the engine is tracking an order the book refused";
  }

  // The two are different failures with different answers: one is the client's
  // price, the other is the venue running out of room, and a client that
  // cannot tell them apart retries the one it should not.
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
      ASSERT_EQ(book.addResting(Side::BUY, resting(id, Side::BUY, 1.0 + 0.01 * double(id), 1.0)),
                BookAddResult::Accepted);
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

// ---- the paths that lift an order off the book and put it back -------------

// A peg target is computed from the touch and is bounded by nothing the ladder
// knows about, so a reference that has run past the band gives a price with no
// level. The order is already off the book when that is discovered -- repeg
// cancels it before reading the market, so the target cannot reference the
// order's own quantity -- and the one thing that must not happen is that it
// disappears without a word.
TEST(LadderBookLiftedOrders, APegRepricedOutsideTheLadderIsCanceledNotLost)
{
  SymbolConfig cfg;
  cfg.id = SYM;
  cfg.tickSize = px(0.01);
  // The collar is deliberately wider than the ladder -- the configuration the
  // book's band exists to survive. It is also what the peg falls back to when
  // its reference is gone: with no touch and no trade the reference is the
  // collar's midpoint, 2500.50, which is a legal price on this instrument and
  // has no level on this ladder.
  cfg.minPrice = px(1.0);
  cfg.maxPrice = px(5000.0);

  Tape tape;
  MatchingEngine<LadderBook> eng(
      cfg, tape.sink(),
      LadderBook{LadderBook::Config{.basePriceRaw = px(1000.0).raw(),
                                    .tickRaw = px(0.01).raw(),
                                    .numLevels = 1000,  // [1000.00, 1010.00)
                                    .maxOrders = 64}});

  // The ask the peg tracks, and the peg two dollars behind it: 1007, in band.
  eng.submit(InboundCommand{limit(1, Side::SELL, 1005.0, 1.0)}, 1);
  NewOrder peg = limit(2, Side::SELL, 1007.0, 1.0);
  peg.accountId = 8;
  peg.peg = PegRef::Ask;
  peg.pegOffsetRaw = px(2.0).raw();
  eng.submit(InboundCommand{peg}, 2);
  ASSERT_EQ(tape.accepts(2), 1u) << "the peg's entry target was in band";

  // The reference goes away. Every submit is a reprice boundary, and this one
  // sends the peg to a price the ladder cannot hold.
  CancelOrder co;
  co.id = 1;
  co.symbol = SYM;
  co.accountId = 7;
  eng.submit(InboundCommand{co}, 3);

  const OrderCanceled* c = tape.canceled(2);
  ASSERT_NE(c, nullptr) << "the peg was lifted off the book and its owner was told nothing";
  EXPECT_EQ(c->reason, CancelReason::BookRefused);
  EXPECT_EQ(c->account, 8u);
  EXPECT_FALSE(eng.book().contains(2)) << "canceled and still on the book";
  EXPECT_EQ(eng.restingOrderCount(), 0u) << "the engine still counts the peg it lost";
  EXPECT_EQ(tape.modified(2), nullptr) << "an OrderModified for an order on no book";
}

// An amend to a price the ladder has no level for. The amend lifts the order
// off the book to re-enter it, so the failure mode is losing the order in
// between: whatever the engine answers, the order is never both gone from the
// book and unreported.
TEST(LadderBookLiftedOrders, AnAmendToAPriceOutsideTheLadderNeverLosesTheOrder)
{
  SymbolConfig cfg;
  cfg.id = SYM;
  cfg.tickSize = px(0.01);

  Tape tape;
  MatchingEngine<LadderBook> eng(
      cfg, tape.sink(),
      LadderBook{LadderBook::Config{.basePriceRaw = px(1000.0).raw(),
                                    .tickRaw = px(0.01).raw(),
                                    .numLevels = 1000,  // [1000.00, 1010.00)
                                    .maxOrders = 64}});

  eng.submit(InboundCommand{limit(1, Side::BUY, 1002.0, 2.0)}, 1);
  ASSERT_EQ(tape.accepts(1), 1u);

  ModifyOrder m;
  m.id = 1;
  m.symbol = SYM;
  m.newPrice = px(1500.0);  // no level on this ladder
  m.newQty = qty(2.0);
  m.accountId = 7;
  eng.submit(InboundCommand{m}, 2);

  const CancelRejected* cr = tape.cancelRejected(1);
  const OrderCanceled* c = tape.canceled(1);
  if (cr != nullptr)
  {
    // Refused before the order was lifted: it is still resting, at its own
    // price, and nothing about it changed.
    EXPECT_EQ(cr->reason, RejectReason::InvalidPrice);
    EXPECT_TRUE(cr->wasReplace);
    ASSERT_TRUE(eng.book().contains(1)) << "the amend was refused and the order is gone anyway";
    EXPECT_EQ(eng.book().find(1)->price.raw(), px(1002.0).raw());
    EXPECT_EQ(eng.restingOrderCount(), 1u);
    EXPECT_EQ(tape.modified(1), nullptr) << "refused, and an OrderModified went out anyway";
    EXPECT_EQ(c, nullptr) << "refused and canceled at the same time";
  }
  else
  {
    // Lifted and refused on the way back: the order is gone, and its owner is
    // told that it is gone rather than that it was modified.
    ASSERT_NE(c, nullptr) << "the amend lifted the order off the book and reported nothing";
    EXPECT_EQ(c->reason, CancelReason::BookRefused);
    EXPECT_FALSE(eng.book().contains(1));
    EXPECT_EQ(eng.restingOrderCount(), 0u) << "the engine still counts the order it lost";
    EXPECT_EQ(tape.modified(1), nullptr) << "an OrderModified for an order on no book";
  }
}

// A last-look reject restores liquidity the hold had taken off the book, and
// the pool can have filled up while the hold was open: the maker's own node
// went back to the free list when the hold took its whole displayed size, and
// other orders are free to claim it. The restore then has nowhere to go.
TEST(LadderBookLiftedOrders, ALastLookRestoreTheBookRefusesCancelsTheMaker)
{
  SymbolConfig cfg;
  cfg.id = SYM;
  cfg.tickSize = px(0.01);
  cfg.lastLookWindowNs = DurationNs{1'000'000};

  Tape tape;
  MatchingEngine<LadderBook> eng(
      cfg, tape.sink(),
      LadderBook{LadderBook::Config{
          .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 100'000, .maxOrders = 2}});

  NewOrder mk = limit(1, Side::SELL, 100.0, 3.0);
  mk.accountId = 1;
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 1);
  NewOrder tk = limit(2, Side::BUY, 100.0, 3.0);
  tk.accountId = 2;
  eng.submit(InboundCommand{tk}, 2);

  const FillHeld* h = tape.held();
  ASSERT_NE(h, nullptr) << "no hold was opened";
  ASSERT_EQ(h->makerDisplayAfter, qty(0.0)) << "the hold must take the maker's whole displayed size";
  // By value: the tape grows under the submits below, and the event it points
  // at moves with it.
  const uint64_t heldId = h->heldId;
  ASSERT_FALSE(eng.book().contains(1)) << "a fully held maker is off the book";

  // The two free nodes -- the maker's own among them -- go to somebody else.
  eng.submit(InboundCommand{limit(3, Side::BUY, 90.0, 1.0)}, 3);
  eng.submit(InboundCommand{limit(4, Side::BUY, 91.0, 1.0)}, 4);
  ASSERT_TRUE(eng.book().full()) << "the pool is what the restore has to fit into";

  eng.submit(InboundCommand{LastLookDecision{heldId, SYM, /*accept=*/false, {}, 1}}, 5);

  const OrderCanceled* c = tape.canceled(1);
  ASSERT_NE(c, nullptr) << "the maker's liquidity was not restored and nobody was told";
  EXPECT_EQ(c->reason, CancelReason::BookRefused);
  EXPECT_EQ(c->account, 1u);
  EXPECT_FALSE(eng.book().contains(1));
  EXPECT_EQ(eng.restingOrderCount(), 2u) << "only the two orders that hold the pool are resting";
  EXPECT_EQ(tape.modified(1), nullptr) << "an OrderModified for a maker on no book";
}

// ---- recovery --------------------------------------------------------------

// A snapshot is restored order by order into a book that may be narrower than
// the one that wrote it -- a ladder reconfigured between runs, a smaller pool.
// An order the book will not take cannot be dropped in silence: the loader
// would then hand back a book one order short of the state whose hash it is
// about to claim, and the SnapshotEnd hash check would have to be the thing
// that noticed.
TEST(LadderBookCheckpoint, ARestoreOrderTheBookCannotTakeIsRefused)
{
  SymbolConfig cfg;
  cfg.id = SYM;
  cfg.tickSize = px(0.01);

  const auto restore = [](OrderId id, Side side, double price, double q)
  {
    RestoreOrder r;
    r.id = id;
    r.accountId = 7;
    r.price = px(price);
    r.leaves = qty(q);
    r.side = side;
    return InboundCommand{r};
  };

  {
    // The ladder spans [1000.00, 1010.00); the snapshot holds an order at 1500.
    Tape tape;
    MatchingEngine<LadderBook> eng(
        cfg, tape.sink(),
        LadderBook{LadderBook::Config{.basePriceRaw = px(1000.0).raw(),
                                      .tickRaw = px(0.01).raw(),
                                      .numLevels = 1000,
                                      .maxOrders = 64}});

    EXPECT_TRUE(eng.applySnapshotRecord(restore(1, Side::BUY, 1002.0, 1.0), 1))
        << "an in-band order must restore";
    EXPECT_FALSE(eng.applySnapshotRecord(restore(2, Side::BUY, 1500.0, 1.0), 2))
        << "an order with no level on this ladder was restored into nothing";
    EXPECT_FALSE(eng.book().contains(2));
    EXPECT_EQ(eng.restingOrderCount(), 1u) << "the engine tracks an order the book never took";
  }

  {
    // Same, for the pool: three orders into room for two.
    Tape tape;
    MatchingEngine<LadderBook> eng(
        cfg, tape.sink(),
        LadderBook{LadderBook::Config{
            .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 100'000, .maxOrders = 2}});

    EXPECT_TRUE(eng.applySnapshotRecord(restore(1, Side::BUY, 10.0, 1.0), 1));
    EXPECT_TRUE(eng.applySnapshotRecord(restore(2, Side::BUY, 11.0, 1.0), 2));
    EXPECT_FALSE(eng.applySnapshotRecord(restore(3, Side::BUY, 12.0, 1.0), 3))
        << "the pool was exhausted and the record was applied anyway";
    EXPECT_FALSE(eng.book().contains(3));
    EXPECT_EQ(eng.restingOrderCount(), 2u) << "the engine tracks an order the book never took";
  }
}
