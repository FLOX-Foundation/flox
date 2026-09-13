/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol test (CONN-06): Bybit option symbols with the "-USDT"
 * quote suffix must parse (the suffix is 5 characters, not 6), and the
 * parsed expiry must land in the wall-clock (unix) domain, not the
 * steady-clock one.
 *
 * parseOptionSymbol is a free function with external linkage defined in
 * bybit_exchange_connector.cpp and not declared in any header (the same
 * function the CONN-06 repro called directly); it is forward-declared here
 * exactly as the repro program did, and resolved from libflox-connectors.a
 * at link time.
 */

#include "flox-connectors/bybit/bybit_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/log/atomic_logger.h>
#include <flox/util/base/time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace flox
{
std::optional<SymbolInfo> parseOptionSymbol(std::string_view fullSymbol,
                                            std::string_view exchange = "bybit");
}

using namespace flox;

namespace
{
std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_bybit_option_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}
}  // namespace

TEST(BybitOptionSymbol, PlainSymbolParsesWithFullFields)
{
  auto info = parseOptionSymbol("BTC-30AUG24-50000-C", "bybit");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->type, InstrumentType::Option);
  ASSERT_TRUE(info->strike.has_value());
  EXPECT_DOUBLE_EQ(info->strike->toDouble(), 50000.0);
  ASSERT_TRUE(info->optionType.has_value());
  EXPECT_EQ(*info->optionType, OptionType::CALL);
  ASSERT_TRUE(info->expiry.has_value());
}

// The regression this test pins: before the fix, ends_with("-USDT") (5
// chars) was followed by remove_suffix(6), eating the trailing option-type
// letter and leaving the type field empty -> nullopt.
TEST(BybitOptionSymbol, UsdtQuoteSuffixParses)
{
  auto info = parseOptionSymbol("BTC-30AUG24-50000-C-USDT", "bybit");
  ASSERT_TRUE(info.has_value()) << "the -USDT suffix (5 chars) must not eat the option-type "
                                   "letter";
  EXPECT_EQ(info->type, InstrumentType::Option);
  ASSERT_TRUE(info->strike.has_value());
  EXPECT_DOUBLE_EQ(info->strike->toDouble(), 50000.0);
  ASSERT_TRUE(info->optionType.has_value());
  EXPECT_EQ(*info->optionType, OptionType::CALL);
}

TEST(BybitOptionSymbol, UsdtSuffixExactLengthDoesNotUB)
{
  // A symbol that is *exactly* the "-USDT" suffix once the rest is
  // stripped away is the boundary case for remove_suffix: length 5 must
  // never underflow. This mirrors the adversarial finding that
  // remove_suffix(6) on a 5-character match is undefined behaviour.
  auto info = parseOptionSymbol("X-1JAN25-1-C-USDT", "bybit");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->type, InstrumentType::Option);
}

// Expiry must be in the wall-clock (unix, via fromUnixMs) domain, not the
// steady-clock one FloxClock::now() returns. A steady-clock reading is
// small (seconds to days since boot); a unix epoch in nanoseconds is
// enormous by comparison, so comparing the parsed expiry against
// FloxClock::now() and a known unix-epoch TimePoint distinguishes the two
// domains without needing to inspect raw counts.
//
// Deliberately does NOT build its "expected" value with flox's own
// fromUnixMs()/now() helpers: both read the same process-global
// unix_to_flox_offset_ns(), so comparing a fromUnixMs() result against
// another fromUnixMs() result (or against now(), which does not depend on
// that offset at all) cannot tell a correctly-offset conversion apart from
// an uninitialized one that happens to cancel out in the comparison. The
// one thing that cannot lie is magnitude: 30-Aug-2024 is roughly 750 days
// before "now" in 2026, a few orders of magnitude below the ~54-year
// (~19700-day) gap the raw duration_cast bug produces (unix nanoseconds
// reinterpreted as a small steady-clock reading). Bounding by days, with
// a wide margin, distinguishes the two without needing either side to
// trust the same helper the bug lives in.
TEST(BybitOptionSymbol, ExpiryIsInWallClockDomain)
{
  auto info = parseOptionSymbol("BTC-30AUG24-50000-C", "bybit");
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->expiry.has_value());

  const auto diff = *info->expiry - now();  // TimePoint (steady_clock) subtraction
  const int64_t diffDays = std::chrono::duration_cast<std::chrono::nanoseconds>(diff).count() /
                           (24LL * 3600 * 1'000'000'000LL);

  // 10-year sanity bound: comfortably covers the true ~2-year gap between
  // 30-Aug-2024 and any run date in the next several years, while
  // excluding the ~54-year magnitude the domain bug produces by more than
  // 5x.
  EXPECT_LT(std::llabs(diffDays), 3650)
      << "expiry is " << diffDays
      << " days from now() -- a magnitude this large means the raw unix-epoch value is still "
         "being read as a steady-clock tick count instead of being offset-mapped";
}

TEST(BybitOptionSymbol, ResolveSymbolIdRegistersUsdtSuffixedOption)
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  OrderExecutionBus orderBus;
  SymbolRegistry registry;
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = "bybit_option_resolve.log";
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  BybitExchangeConnector connector(cfg, &bookBus, &tradeBus, &orderBus, &registry, logger);

  SymbolId id = connector.resolveSymbolId("BTC-30AUG24-50000-C-USDT");
  auto info = registry.getSymbolInfo(id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->type, InstrumentType::Option)
      << "a -USDT-suffixed option symbol must not fall back to Spot";
  ASSERT_TRUE(info->strike.has_value());
  EXPECT_DOUBLE_EQ(info->strike->toDouble(), 50000.0);
}
