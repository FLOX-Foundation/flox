#include <gtest/gtest.h>

#include <chrono>

#include "flox/backtest/option_settlement_engine.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_group.h"

using namespace flox;

namespace
{
constexpr int64_t kExpiryNs = 1'700'000'000'000'000'000LL;

// Register a cash-settled European option and return its SymbolId.
SymbolId registerOption(SymbolRegistry& reg, OptionType type, double strike, int mult = 1)
{
  SymbolInfo info;
  info.exchange = "deribit";
  info.symbol = "BTC-OPT";
  info.type = InstrumentType::Option;
  info.strike = Price::fromDouble(strike);
  info.optionType = type;
  info.expiry = TimePoint(std::chrono::nanoseconds(kExpiryNs));
  info.settlementType = SettlementType::Cash;
  info.contractMultiplier = static_cast<double>(mult);
  return reg.registerSymbol(info);
}
}  // namespace

TEST(OptionSettlementTest, ItmCallSettlesToIntrinsic)
{
  SymbolRegistry reg;
  SymbolId sym = registerOption(reg, OptionType::CALL, 100.0);

  PositionGroupTracker positions;
  // Long call, premium 5, 2 contracts.
  positions.openPosition(1, sym, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionSettlementEngine engine(reg);
  // Index 120 at expiry -> intrinsic 20 -> PnL = (20 - 5) * 2 = 30.
  size_t n = engine.settleExpired(kExpiryNs, positions, [](SymbolId)
                                  { return 120.0; });
  EXPECT_EQ(n, 1u);
  EXPECT_EQ(positions.openPositionCount(), 0);
  EXPECT_NEAR(positions.totalRealizedPnl().toDouble(), 30.0, 0.01);
}

TEST(OptionSettlementTest, ItmPutSettlesToIntrinsic)
{
  SymbolRegistry reg;
  SymbolId sym = registerOption(reg, OptionType::PUT, 100.0);

  PositionGroupTracker positions;
  positions.openPosition(1, sym, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionSettlementEngine engine(reg);
  // Index 80 -> put intrinsic 20 -> PnL = (20 - 5) * 2 = 30.
  engine.settleExpired(kExpiryNs, positions, [](SymbolId)
                       { return 80.0; });
  EXPECT_NEAR(positions.totalRealizedPnl().toDouble(), 30.0, 0.01);
}

TEST(OptionSettlementTest, OtmExpiresWorthless)
{
  SymbolRegistry reg;
  SymbolId sym = registerOption(reg, OptionType::CALL, 100.0);

  PositionGroupTracker positions;
  positions.openPosition(1, sym, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionSettlementEngine engine(reg);
  // Index 90 < strike -> intrinsic 0 -> long loses premium: (0 - 5) * 2 = -10.
  engine.settleExpired(kExpiryNs, positions, [](SymbolId)
                       { return 90.0; });
  EXPECT_EQ(positions.openPositionCount(), 0);
  EXPECT_NEAR(positions.totalRealizedPnl().toDouble(), -10.0, 0.01);
}

TEST(OptionSettlementTest, MultiplierScalesSettlement)
{
  SymbolRegistry reg;
  SymbolId sym = registerOption(reg, OptionType::CALL, 100.0, /*mult=*/10);

  PositionGroupTracker positions;
  positions.openPosition(1, sym, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0), 10.0);

  OptionSettlementEngine engine(reg);
  // (20 - 5) * 2 * 10 = 300.
  engine.settleExpired(kExpiryNs, positions, [](SymbolId)
                       { return 120.0; });
  EXPECT_NEAR(positions.totalRealizedPnl().toDouble(), 300.0, 0.01);
}

TEST(OptionSettlementTest, NotYetExpiredIsNotSettled)
{
  SymbolRegistry reg;
  SymbolId sym = registerOption(reg, OptionType::CALL, 100.0);

  PositionGroupTracker positions;
  positions.openPosition(1, sym, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionSettlementEngine engine(reg);
  // now is one second before expiry.
  size_t n = engine.settleExpired(kExpiryNs - 1'000'000'000LL, positions,
                                  [](SymbolId)
                                  { return 120.0; });
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(positions.openPositionCount(), 1);
}

TEST(OptionSettlementTest, SettlesOnceNoDoubleSettle)
{
  SymbolRegistry reg;
  SymbolId sym = registerOption(reg, OptionType::CALL, 100.0);

  PositionGroupTracker positions;
  positions.openPosition(1, sym, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(2.0));

  OptionSettlementEngine engine(reg);
  EXPECT_EQ(engine.settleExpired(kExpiryNs, positions, [](SymbolId)
                                 { return 120.0; }),
            1u);
  // A second pass finds the position already closed and settles nothing.
  EXPECT_EQ(engine.settleExpired(kExpiryNs, positions, [](SymbolId)
                                 { return 120.0; }),
            0u);
  EXPECT_NEAR(positions.totalRealizedPnl().toDouble(), 30.0, 0.01);
}

TEST(OptionSettlementTest, NonOptionPositionIgnored)
{
  SymbolRegistry reg;
  SymbolInfo perp;
  perp.exchange = "bybit";
  perp.symbol = "BTCUSDT";
  perp.type = InstrumentType::Future;
  SymbolId sym = reg.registerSymbol(perp);

  PositionGroupTracker positions;
  positions.openPosition(1, sym, Side::BUY, Price::fromDouble(100.0), Quantity::fromDouble(2.0));

  OptionSettlementEngine engine(reg);
  EXPECT_EQ(engine.settleExpired(kExpiryNs, positions, [](SymbolId)
                                 { return 120.0; }),
            0u);
  EXPECT_EQ(positions.openPositionCount(), 1);
}
