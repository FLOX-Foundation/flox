/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/position/aggregated_position_tracker.h"
#include "flox/position/position_valuator.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

// A custom valuator that ignores the linear formula and returns a fixed
// value, standing in for a nonlinear (LP / option) valuation.
struct FixedValuator : IPositionValuator
{
  Volume value;
  explicit FixedValuator(Volume v) : value(v) {}
  Volume unrealizedPnl(SymbolId, Quantity, Price, Price) const override { return value; }
};

TEST(PositionValuatorTest, DefaultIsLinear)
{
  AggregatedPositionTracker<> tracker;
  tracker.onFill(0, 1, Quantity::fromDouble(2.0), Price::fromDouble(100.0));

  // Linear: qty * (current - avg) = 2 * (110 - 100) = 20.
  Volume pnl = tracker.unrealizedPnl(1, Price::fromDouble(110.0));
  EXPECT_NEAR(pnl.toDouble(), 20.0, 1e-6);
}

TEST(PositionValuatorTest, CustomValuatorOverridesLinear)
{
  AggregatedPositionTracker<> tracker;
  tracker.onFill(0, 1, Quantity::fromDouble(2.0), Price::fromDouble(100.0));

  FixedValuator fv(Volume::fromDouble(999.0));
  tracker.setValuator(&fv);

  Volume pnl = tracker.unrealizedPnl(1, Price::fromDouble(110.0));
  EXPECT_NEAR(pnl.toDouble(), 999.0, 1e-6);
}

TEST(PositionValuatorTest, ResettingValuatorRestoresLinear)
{
  AggregatedPositionTracker<> tracker;
  tracker.onFill(0, 1, Quantity::fromDouble(2.0), Price::fromDouble(100.0));

  FixedValuator fv(Volume::fromDouble(999.0));
  tracker.setValuator(&fv);
  tracker.setValuator(nullptr);

  Volume pnl = tracker.unrealizedPnl(1, Price::fromDouble(110.0));
  EXPECT_NEAR(pnl.toDouble(), 20.0, 1e-6);
}

// A set valuator is consulted even with no linear position: a nonlinear
// valuator (AMM LP, option) derives value from its own state, not from a
// tracked quantity.
TEST(PositionValuatorTest, ValuatorConsultedAtZeroPosition)
{
  AggregatedPositionTracker<> tracker;  // no onFill -> zero linear position
  FixedValuator fv(Volume::fromDouble(999.0));
  tracker.setValuator(&fv);
  EXPECT_NEAR(tracker.unrealizedPnl(1, Price::fromDouble(110.0)).toDouble(), 999.0, 1e-6);
}

// The linear default still returns zero for a flat position.
TEST(PositionValuatorTest, LinearFlatPositionIsZero)
{
  AggregatedPositionTracker<> tracker;
  EXPECT_EQ(tracker.unrealizedPnl(1, Price::fromDouble(110.0)).raw(), 0);
}

}  // namespace
