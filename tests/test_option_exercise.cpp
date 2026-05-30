#include <gtest/gtest.h>

#include "flox/backtest/option_exercise_engine.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_group.h"

#include <string>

using namespace flox;

namespace
{
int g_optSeq = 0;

SymbolId registerOption(SymbolRegistry& reg, OptionType type, double strike,
                        ExerciseStyle style, int mult = 1)
{
  SymbolInfo info;
  info.exchange = "cme";
  info.symbol = "OPT-" + std::to_string(g_optSeq++);  // unique per registration
  info.type = InstrumentType::Option;
  info.strike = Price::fromDouble(strike);
  info.optionType = type;
  info.exerciseStyle = style;
  info.contractMultiplier = static_cast<double>(mult);
  return reg.registerSymbol(info);
}

SymbolId registerUnderlying(SymbolRegistry& reg)
{
  SymbolInfo info;
  info.exchange = "cme";
  info.symbol = "UND";
  info.type = InstrumentType::Spot;
  return reg.registerSymbol(info);
}
}  // namespace

// Exercising a long American call buys the underlying at the strike and closes
// the option leg.
TEST(OptionExerciseTest, LongCallExerciseOpensUnderlyingLong)
{
  SymbolRegistry reg;
  const SymbolId opt = registerOption(reg, OptionType::CALL, 100.0, ExerciseStyle::American);
  const SymbolId und = registerUnderlying(reg);

  PositionGroupTracker positions;
  const PositionId optPid =
      positions.openPosition(1, opt, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionExerciseEngine engine(reg);
  const auto res = engine.exercise(positions, optPid, und);

  ASSERT_TRUE(res.exercised);
  EXPECT_TRUE(positions.getPosition(optPid)->closed);
  const IndividualPosition* u = positions.getPosition(res.underlyingPositionId);
  ASSERT_NE(u, nullptr);
  EXPECT_EQ(u->side, Side::BUY);
  EXPECT_NEAR(u->entryPrice.toDouble(), 100.0, 1e-9);
  EXPECT_NEAR(u->quantity.toDouble(), 2.0, 1e-9);
}

// A long put exercises into a short underlying position (sell at strike).
TEST(OptionExerciseTest, LongPutExerciseOpensUnderlyingShort)
{
  SymbolRegistry reg;
  const SymbolId opt = registerOption(reg, OptionType::PUT, 100.0, ExerciseStyle::American);
  const SymbolId und = registerUnderlying(reg);

  PositionGroupTracker positions;
  const PositionId optPid =
      positions.openPosition(1, opt, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(3.0));

  OptionExerciseEngine engine(reg);
  const auto res = engine.exercise(positions, optPid, und);

  ASSERT_TRUE(res.exercised);
  const IndividualPosition* u = positions.getPosition(res.underlyingPositionId);
  ASSERT_NE(u, nullptr);
  EXPECT_EQ(u->side, Side::SELL);
  EXPECT_NEAR(u->quantity.toDouble(), 3.0, 1e-9);
}

// Assignment on a short call forces a short underlying delivery (sell at strike),
// the mirror of a long call's exercise.
TEST(OptionExerciseTest, ShortCallAssignmentOpensUnderlyingShort)
{
  SymbolRegistry reg;
  const SymbolId opt = registerOption(reg, OptionType::CALL, 100.0, ExerciseStyle::American);
  const SymbolId und = registerUnderlying(reg);

  PositionGroupTracker positions;
  // Short call: sold for premium 5.
  const PositionId optPid =
      positions.openPosition(1, opt, Side::SELL, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionExerciseEngine engine(reg);
  const auto res = engine.exercise(positions, optPid, und);

  ASSERT_TRUE(res.exercised);
  const IndividualPosition* u = positions.getPosition(res.underlyingPositionId);
  ASSERT_NE(u, nullptr);
  EXPECT_EQ(u->side, Side::SELL);
  // Short keeps the premium when the option closes at zero: (0 - 5) on a SELL = +10.
  EXPECT_NEAR(positions.realizedPnl(opt).toDouble(), 10.0, 1e-9);
}

// Short put assignment forces a long underlying delivery (buy at strike).
TEST(OptionExerciseTest, ShortPutAssignmentOpensUnderlyingLong)
{
  SymbolRegistry reg;
  const SymbolId opt = registerOption(reg, OptionType::PUT, 100.0, ExerciseStyle::American);
  const SymbolId und = registerUnderlying(reg);

  PositionGroupTracker positions;
  const PositionId optPid =
      positions.openPosition(1, opt, Side::SELL, Price::fromDouble(5.0), Quantity::fromDouble(1.0));

  OptionExerciseEngine engine(reg);
  const auto res = engine.exercise(positions, optPid, und);

  ASSERT_TRUE(res.exercised);
  EXPECT_EQ(positions.getPosition(res.underlyingPositionId)->side, Side::BUY);
}

// European options carry no early-exercise right: exercise refuses them and the
// option position stays open.
TEST(OptionExerciseTest, EuropeanRejected)
{
  SymbolRegistry reg;
  const SymbolId opt = registerOption(reg, OptionType::CALL, 100.0, ExerciseStyle::European);
  const SymbolId und = registerUnderlying(reg);

  PositionGroupTracker positions;
  const PositionId optPid =
      positions.openPosition(1, opt, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionExerciseEngine engine(reg);
  const auto res = engine.exercise(positions, optPid, und);

  EXPECT_FALSE(res.exercised);
  EXPECT_FALSE(positions.getPosition(optPid)->closed);
  EXPECT_EQ(positions.openPositionCount(), 1u);
}

// The contract multiplier scales the delivered underlying quantity: one
// 100-multiplier option delivers 100 underlying units per contract.
TEST(OptionExerciseTest, MultiplierScalesDeliveredUnits)
{
  SymbolRegistry reg;
  const SymbolId opt = registerOption(reg, OptionType::CALL, 100.0, ExerciseStyle::American, 100);
  const SymbolId und = registerUnderlying(reg);

  PositionGroupTracker positions;
  const PositionId optPid =
      positions.openPosition(1, opt, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionExerciseEngine engine(reg);
  const auto res = engine.exercise(positions, optPid, und);

  ASSERT_TRUE(res.exercised);
  EXPECT_NEAR(positions.getPosition(res.underlyingPositionId)->quantity.toDouble(), 200.0, 1e-6);
}

// Exercise then mark the underlying to spot: total realized PnL is
// intrinsic - premium, with no double counting of the intrinsic.
TEST(OptionExerciseTest, EndToEndPnlIsIntrinsicMinusPremium)
{
  SymbolRegistry reg;
  const SymbolId opt = registerOption(reg, OptionType::CALL, 100.0, ExerciseStyle::American);
  const SymbolId und = registerUnderlying(reg);

  PositionGroupTracker positions;
  const PositionId optPid =
      positions.openPosition(1, opt, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionExerciseEngine engine(reg);
  const auto res = engine.exercise(positions, optPid, und);
  ASSERT_TRUE(res.exercised);

  // Underlying now worth 120: close it to realize the delivery gain.
  positions.closePosition(res.underlyingPositionId, Price::fromDouble(120.0));

  // (intrinsic 20 - premium 5) * 2 contracts = 30.
  EXPECT_NEAR(positions.totalRealizedPnl().toDouble(), 30.0, 1e-9);
}

// The BAW boundary check flags a deep-ITM American put for early exercise while
// leaving an out-of-the-money one alone; European always returns false.
TEST(OptionExerciseTest, EarlyExerciseBoundary)
{
  SymbolRegistry reg;
  const SymbolId amerPut = registerOption(reg, OptionType::PUT, 100.0, ExerciseStyle::American);
  const SymbolId euroPut = registerOption(reg, OptionType::PUT, 100.0, ExerciseStyle::European);

  OptionExerciseEngine engine(reg);
  const double t = 0.5, r = 0.10, b = 0.0, vol = 0.25;

  EXPECT_TRUE(engine.isEarlyExerciseOptimal(amerPut, /*spot=*/40.0, t, r, b, vol));    // deep ITM
  EXPECT_FALSE(engine.isEarlyExerciseOptimal(amerPut, /*spot=*/110.0, t, r, b, vol));  // OTM
  EXPECT_FALSE(engine.isEarlyExerciseOptimal(euroPut, /*spot=*/40.0, t, r, b, vol));   // European
}
