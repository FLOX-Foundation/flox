/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * What a shard costs, and what that cost does NOT change.
 *
 * A SequencedShard carries its ingress and outbound rings by value, so the
 * capacity picked at instantiation is the object's size. At the default that
 * is thirty megabytes, which has two consequences nothing in the tree stated:
 * a shard does not fit in a stack frame, and a venue of a hundred instruments
 * is three gigabytes of ring before a single order arrives.
 *
 * Both are fine as long as they are chosen. This file pins the numbers so a
 * change to the default or to an event struct shows up here rather than in
 * someone's memory graph, and proves the one thing that must not depend on
 * capacity: the events a shard produces.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr size_t kMiB = 1024u * 1024u;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct)
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

struct HashSink : IEngineEventListener
{
  uint64_t h = 0;
  uint64_t count = 0;
  void onEngineEvent(const EngineEventMsg& e) override
  {
    h = hashEvent(h, e.event);
    ++count;
  }
};

std::vector<InboundCommand> script()
{
  std::vector<InboundCommand> v;
  for (int i = 0; i < 200; ++i)
  {
    const Side s = (i % 2) == 0 ? Side::SELL : Side::BUY;
    v.push_back(InboundCommand{limit(static_cast<OrderId>(i + 1), s, 100.0 + ((i % 5) - 2) * 0.01, 1,
                                     7 + (i % 3))});
  }
  return v;
}

// Drive a shard of the given capacity through the script and return the
// digest of everything it published.
template <size_t Cap>
HashSink drive()
{
  const std::string path = tmpPath("shard-cap");
  auto shard = std::make_unique<SequencedShard<MatchingBook, Cap, Cap>>(cfg(), path);
  HashSink sink;
  shard->subscribeOutbound(&sink);
  shard->start();
  for (const auto& c : script())
  {
    shard->submit(c);
  }
  shard->stop();
  std::remove(path.c_str());
  return sink;
}

}  // namespace

// The number, in the open. A shard is not a stack object and the default is
// not free; both facts live here rather than in a surprise.
TEST(ShardCapacity, TheDefaultShardIsTensOfMegabytesAndSaysSo)
{
  const size_t big = sizeof(SequencedShard<MatchingBook>);
  const size_t small = sizeof(SequencedShard<MatchingBook, 1u << 12, 1u << 12>);
  const size_t tiny = sizeof(SequencedShard<MatchingBook, 1u << 10, 1u << 10>);
  std::printf("  shard size: default %.2f MiB, 4096 %.2f MiB, 1024 %.2f MiB\n",
              static_cast<double>(big) / kMiB, static_cast<double>(small) / kMiB,
              static_cast<double>(tiny) / kMiB);
  std::printf("  one ingress event %zu bytes, one outbound event %zu bytes\n",
              sizeof(InboundCommandEvent), sizeof(EngineEventMsg));

  // A default shard is far past any thread's stack. Stated as a fact about
  // the object so that lowering the default, or shrinking an event struct,
  // has to come through this line.
  EXPECT_GT(big, 8 * kMiB) << "if this is no longer true the heap rule can be relaxed -- "
                              "check the documentation before changing the number";

  // And the capacity is what dominates it: a shard sized for a quiet
  // instrument is a couple of megabytes, not tens.
  EXPECT_LT(small, 4 * kMiB);
  EXPECT_LT(tiny, 1 * kMiB);
  EXPECT_LT(tiny, small);
  EXPECT_LT(small, big);
}

// Capacity is a buffering decision. It must not reach the events: a venue that
// produced different history because a ring was sized differently could not be
// replayed, and every determinism guarantee in the tree rests on that.
TEST(ShardCapacity, TheEventsAShardProducesDoNotDependOnItsRingSize)
{
  const HashSink big = drive<1u << 16>();
  const HashSink small = drive<1u << 12>();
  const HashSink tiny = drive<1u << 10>();

  EXPECT_GT(big.count, 0u);
  EXPECT_EQ(small.count, big.count);
  EXPECT_EQ(tiny.count, big.count);
  EXPECT_EQ(small.h, big.h) << "a smaller ring changed the history";
  EXPECT_EQ(tiny.h, big.h) << "a smaller ring changed the history";
}
