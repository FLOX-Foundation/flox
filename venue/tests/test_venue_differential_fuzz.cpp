/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Differential fuzz over the two order-level matching books. Drives a large
 * mixed random command stream (limits both sides, markets, IOC, post-only,
 * cancels, modifies, pegs) through the std::map reference book and the O(1)
 * ladder book in lockstep, asserting after every command that:
 *   - both engines emitted an identical event stream (rolling hash), and
 *   - neither book is crossed (best bid < best ask).
 * The reference book is the oracle; any ladder divergence fails with the command
 * index. Reproducible: fixed xorshift seed, no wall-clock / RNG.
 *
 * Depth defaults to a CI-friendly size; set FLOX_FUZZ_OPS for a deep run
 * (the venue application gates 2M under ASAN/UBSAN before release).
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox/book/ladder_book.h"
#include "support/counterexample.h"

#include <gtest/gtest.h>
#include <cstdint>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
constexpr SymbolId SYM = 1;

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  return c;
}

LadderBook::Config ladderCfg()
{
  return LadderBook::Config{/*base*/ 0, /*tick*/ Price::fromDouble(0.01).raw(),
                            /*levels*/ 16000, /*maxOrders*/ 1 << 20};
}

template <class Book>
bool crossed(const Book& b)
{
  const auto bid = b.bestBid();
  const auto ask = b.bestAsk();
  return bid.has_value() && ask.has_value() && !(bid.value() < ask.value());
}

InboundCommand genCommand(uint64_t& s, uint64_t seq, OrderId& nextId)
{
  auto next = [&]() noexcept
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  const uint64_t r = next();
  const int64_t midRaw = Price::fromDouble(100.0).raw();
  const int64_t tickRaw = Price::fromDouble(0.01).raw();
  const uint32_t kind = r % 100;

  if (kind < 15 && nextId > 1)
  {
    // cancel a random earlier id (may be live or already gone -- both books
    // must agree either way)
    const OrderId victim = 1 + (next() % (nextId - 1));
    return CancelOrder{victim, SYM, 1};
  }
  if (kind < 25 && nextId > 1)
  {
    // modify a random earlier id: new price within band, new qty 1..6
    const OrderId victim = 1 + (next() % (nextId - 1));
    const int ticks = static_cast<int>((next() % 101)) - 50;
    const Price newPrice = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
    const Quantity newQty = Quantity::fromDouble(1.0 + static_cast<double>(next() % 6));
    return ModifyOrder{victim, SYM, newPrice, newQty, 1};
  }

  NewOrder o;
  o.id = nextId++;
  o.symbol = SYM;
  o.side = (r & 1U) ? Side::BUY : Side::SELL;
  o.quantity = Quantity::fromDouble(1.0 + static_cast<double>((r >> 10) % 5));
  o.accountId = 1 + ((r >> 20) % 8);

  if (kind < 30)
  {
    o.type = OrderType::MARKET;
  }
  else if (kind < 40)
  {
    // stop-market with a random in-band trigger (engine-level, deterministic;
    // both books must still agree, and cascades are exercised)
    o.type = OrderType::STOP_MARKET;
    const int ticks = static_cast<int>((r >> 1) % 101) - 50;
    o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
  }
  else
  {
    o.type = OrderType::LIMIT;
    const int ticks = static_cast<int>((r >> 1) % 101) - 50;  // +/-50 ticks
    o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
    const uint32_t t = (r >> 32) % 100;
    if (t < 12)
    {
      o.tif = TimeInForce::IOC;
    }
    else if (t < 17)
    {
      o.postOnly = true;
    }
    else if (t < 30)
    {
      o.visibleQuantity = Quantity::fromDouble(1.0);  // iceberg peak 1 (if qty > 1)
    }
    else if (t < 42)
    {
      o.tif = TimeInForce::GTD;  // some expire, some never
      o.expiryNs = SeqNanos::fromRaw(static_cast<int64_t>((r >> 40) % 2'000'000));
    }
    else if (t < 50)
    {
      o.ocoGroup = 1 + ((r >> 44) % 6);  // small set -> siblings collide
    }
    else if (t < 60)
    {
      const uint32_t pr = static_cast<uint32_t>((r >> 48) % 3);
      o.peg = pr == 0 ? PegRef::Bid : (pr == 1 ? PegRef::Ask : PegRef::Mid);
    }
  }
  (void)seq;
  return InboundCommand{o};
}

int g_failures = 0;

// Pure replay used by the minimiser: fresh engines every time, same three
// comparisons as the live loop. Nothing is carried over between calls.
bool diverges(const std::vector<InboundCommand>& cmds)
{
  uint64_t hRef = 1469598103934665603ULL;
  uint64_t hLad = hRef;
  std::vector<OutboundEvent> evRef;
  std::vector<OutboundEvent> evLad;
  MatchingEngine<MatchingBook> refE(cfg(), [&](const OutboundEvent& e)
                                    { evRef.push_back(e); });
  MatchingEngine<LadderBook> ladE(cfg(), [&](const OutboundEvent& e)
                                  { evLad.push_back(e); }, LadderBook{ladderCfg()});
  for (const InboundCommand& cmd : cmds)
  {
    evRef.clear();
    evLad.clear();
    refE.submit(cmd);
    ladE.submit(cmd);
    if (evRef.size() != evLad.size())
    {
      return true;
    }
    for (size_t k = 0; k < evRef.size(); ++k)
    {
      hRef = hashEvent(hRef, evRef[k]);
      hLad = hashEvent(hLad, evLad[k]);
    }
    if (hRef != hLad || crossed(refE.book()) || crossed(ladE.book()))
    {
      return true;
    }
  }
  return false;
}
}  // namespace

TEST(VenueDifferentialFuzz, LadderMatchesReferenceBook)
{
  uint64_t hRef = 1469598103934665603ULL;
  uint64_t hLad = hRef;
  std::vector<OutboundEvent> evRef;
  std::vector<OutboundEvent> evLad;

  MatchingEngine<MatchingBook> refE(cfg(), [&](const OutboundEvent& e)
                                    { evRef.push_back(e); });
  MatchingEngine<LadderBook> ladE(cfg(), [&](const OutboundEvent& e)
                                  { evLad.push_back(e); }, LadderBook{ladderCfg()});

  const char* env = std::getenv("FLOX_FUZZ_OPS");
  const uint64_t N = env != nullptr ? std::strtoull(env, nullptr, 10) : 200'000;
  uint64_t s = 0xC0FFEE123456789ULL;
  OrderId nextId = 1;

  // The stream is recorded so a failure can be handed over minimised instead
  // of as an index into 200k commands.
  std::vector<InboundCommand> stream;
  stream.reserve(N);

  for (uint64_t i = 0; i < N; ++i)
  {
    const InboundCommand cmd = genCommand(s, i, nextId);
    stream.push_back(cmd);
    evRef.clear();
    evLad.clear();
    refE.submit(cmd);
    ladE.submit(cmd);

    if (evRef.size() != evLad.size())
    {
      std::printf("FAIL cmd %llu: event count %zu != %zu\n",
                  static_cast<unsigned long long>(i), evRef.size(), evLad.size());
      ++g_failures;
      break;
    }
    for (size_t k = 0; k < evRef.size(); ++k)
    {
      hRef = hashEvent(hRef, evRef[k]);
      hLad = hashEvent(hLad, evLad[k]);
    }
    if (hRef != hLad)
    {
      std::printf("FAIL cmd %llu: event hash diverged\n", static_cast<unsigned long long>(i));
      ++g_failures;
      break;
    }
    if (crossed(refE.book()) || crossed(ladE.book()))
    {
      std::printf("FAIL cmd %llu: crossed book\n", static_cast<unsigned long long>(i));
      ++g_failures;
      break;
    }
  }

  if (g_failures == 0)
  {
    std::printf("differential: %llu commands, ladder == reference, no crossed book\n",
                static_cast<unsigned long long>(N));
    std::printf("final event-stream hash: %016llx\n", static_cast<unsigned long long>(hRef));
  }
  else
  {
    const auto report = fuzz::shrinkCounterexample(stream, diverges);
    fuzz::printCounterexample(report);
  }

  EXPECT_EQ(g_failures, 0);
}

// The minimiser is only exercised when the fuzz fails, which is exactly when
// nobody wants to discover it does not work. This drives it against a
// synthetic predicate with a known-minimal witness: a stream fails when it
// holds both a SELL and a modify, so the smallest failing stream is those two
// commands and nothing else.
TEST(VenueDifferentialFuzz, MinimiserReducesToTheWitness)
{
  std::vector<InboundCommand> stream;
  for (int i = 0; i < 500; ++i)
  {
    NewOrder o;
    o.id = static_cast<OrderId>(i + 1);
    o.symbol = SYM;
    o.side = (i == 137) ? Side::SELL : Side::BUY;
    o.type = OrderType::LIMIT;
    o.price = Price::fromDouble(99.0);
    o.quantity = Quantity::fromDouble(1.0);
    o.accountId = 1;
    stream.push_back(InboundCommand{o});
    if (i == 400)
    {
      stream.push_back(InboundCommand{ModifyOrder{1, SYM, Price::fromDouble(98.0),
                                                  Quantity::fromDouble(2.0), 0}});
    }
  }

  auto witnessPresent = [](const std::vector<InboundCommand>& cmds)
  {
    bool sell = false, modify = false;
    for (const auto& c : cmds)
    {
      if (const auto* o = std::get_if<NewOrder>(&c); o && o->side == Side::SELL)
      {
        sell = true;
      }
      if (std::get_if<ModifyOrder>(&c) != nullptr)
      {
        modify = true;
      }
    }
    return sell && modify;
  };

  ASSERT_TRUE(witnessPresent(stream));
  const auto report = fuzz::shrinkCounterexample(stream, witnessPresent);

  EXPECT_TRUE(witnessPresent(report.commands)) << "minimiser lost the failure it was shrinking";
  EXPECT_EQ(report.commands.size(), 2u) << "did not reach the two-command witness";
  EXPECT_GT(report.replays, 0) << "minimiser never replayed anything";
  EXPECT_LT(report.replays, 400) << "hit the replay cap on a 501-command stream";
}
