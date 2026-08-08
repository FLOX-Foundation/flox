/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include "flox/strategy/shadow.h"

using flox::Price;
using flox::Quantity;
using flox::ShadowComparator;
using flox::ShadowSignalHandler;
using flox::Side;
using flox::Signal;
using flox::SignalType;

namespace
{

Signal makeSignal(SignalType type, uint32_t symbol, Side side, double price, double qty)
{
  Signal s;
  s.type = type;
  s.symbol = symbol;
  s.side = side;
  s.price = Price::fromDouble(price);
  s.quantity = Quantity::fromDouble(qty);
  return s;
}

class CountingInner : public flox::ISignalHandler
{
 public:
  void onSignal(const Signal&) override { ++count; }
  int count{0};
};

}  // namespace

TEST(ShadowSignalHandler, RecordsAndSuppressesExecution)
{
  ShadowSignalHandler shadow;
  shadow.onSignal(makeSignal(SignalType::Limit, 1, Side::BUY, 100.5, 2.0));
  shadow.onSignal(makeSignal(SignalType::Cancel, 1, Side::SELL, 0.0, 0.0));

  ASSERT_EQ(shadow.records().size(), 2u);
  EXPECT_EQ(shadow.records()[0].type, SignalType::Limit);
  EXPECT_EQ(shadow.records()[0].priceRaw, Price::fromDouble(100.5).raw());
  EXPECT_GT(shadow.records()[0].tsNs, 0);
  // No inner handler: nothing was executed, by construction.
}

TEST(ShadowSignalHandler, CanaryModeForwards)
{
  CountingInner inner;
  ShadowSignalHandler shadow(100, &inner);
  shadow.onSignal(makeSignal(SignalType::Market, 1, Side::BUY, 0.0, 1.0));
  EXPECT_EQ(inner.count, 1);
  EXPECT_EQ(shadow.records().size(), 1u);
}

TEST(ShadowSignalHandler, BoundedMemoryEvictsOldest)
{
  ShadowSignalHandler shadow(/*maxRecords=*/3);
  for (int i = 0; i < 5; ++i)
  {
    shadow.onSignal(makeSignal(SignalType::Market, uint32_t(i), Side::BUY, 0.0, 1.0));
  }
  EXPECT_EQ(shadow.records().size(), 3u);
  EXPECT_EQ(shadow.evicted(), 2u);
  EXPECT_EQ(shadow.records().front().symbol, 2u);  // oldest two evicted
}

TEST(ShadowComparator, IdenticalStreamsAreClean)
{
  ShadowSignalHandler ref, cand;
  for (int i = 0; i < 10; ++i)
  {
    const auto s = makeSignal(SignalType::Limit, 1, Side::BUY, 100.0 + i, 1.0);
    ref.onSignal(s);
    cand.onSignal(s);
  }

  ShadowComparator cmp;
  const auto rep = cmp.compare(ref, cand);
  EXPECT_TRUE(rep.clean());
  EXPECT_EQ(rep.matched, 10u);
  EXPECT_EQ(rep.latencyDelta->count(), 10u);
}

TEST(ShadowComparator, DivergenceIsLocatedAndDescribed)
{
  ShadowSignalHandler ref, cand;
  ref.onSignal(makeSignal(SignalType::Limit, 1, Side::BUY, 100.0, 1.0));
  cand.onSignal(makeSignal(SignalType::Limit, 1, Side::BUY, 100.0, 1.0));
  ref.onSignal(makeSignal(SignalType::Limit, 1, Side::BUY, 101.0, 1.0));
  cand.onSignal(makeSignal(SignalType::Limit, 1, Side::SELL, 101.0, 1.0));  // diverges

  ShadowComparator cmp;
  const auto rep = cmp.compare(ref, cand);
  EXPECT_FALSE(rep.clean());
  ASSERT_EQ(rep.divergences.size(), 1u);
  EXPECT_EQ(rep.divergences[0].index, 1u);
  EXPECT_EQ(rep.divergences[0].what, "side mismatch");
  EXPECT_EQ(rep.matched, 1u);
}

TEST(ShadowComparator, CountMismatchIsReported)
{
  ShadowSignalHandler ref, cand;
  ref.onSignal(makeSignal(SignalType::Market, 1, Side::BUY, 0.0, 1.0));
  ref.onSignal(makeSignal(SignalType::Market, 1, Side::BUY, 0.0, 1.0));
  cand.onSignal(makeSignal(SignalType::Market, 1, Side::BUY, 0.0, 1.0));

  ShadowComparator cmp;
  const auto rep = cmp.compare(ref, cand);
  EXPECT_FALSE(rep.clean());
  ASSERT_EQ(rep.divergences.size(), 1u);
  EXPECT_NE(rep.divergences[0].what.find("count mismatch"), std::string::npos);
}
