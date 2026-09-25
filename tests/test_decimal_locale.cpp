/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Decimal::toString() is what every connector writes into an order body, so
// its output is a wire format, not a display string. It used to go through
// std::to_string(double), which writes the C locale's decimal separator: on a
// host configured for comma decimals every price and quantity left as
// "60000,000000" and the venue rejected the order.

#include <flox/common.h>
#include <flox/util/base/decimal.h>

#include <gtest/gtest.h>

#include <clocale>
#include <locale>
#include <string>

using namespace flox;

namespace
{

// Restores both the C++ global locale and the C locale: std::locale::global
// sets the C locale as well when the locale is named.
class ScopedGlobalLocale
{
 public:
  ScopedGlobalLocale() : _prev(std::locale()) {}

  ~ScopedGlobalLocale()
  {
    std::locale::global(_prev);
    std::setlocale(LC_ALL, "C");
  }

  // Installs the first comma-decimal locale that exists on this host, or
  // returns an empty string if it carries none of them.
  std::string installCommaDecimal()
  {
    for (const char* name : {"fr_FR.UTF-8", "ru_RU.UTF-8", "it_IT.UTF-8", "es_ES.UTF-8",
                             "pt_BR.UTF-8", "fr_FR.utf8", "ru_RU.utf8", "it_IT.utf8"})
    {
      try
      {
        std::locale::global(std::locale(name));
        return name;
      }
      catch (const std::runtime_error&)
      {
        continue;
      }
    }
    return {};
  }

 private:
  std::locale _prev;
};

}  // namespace

TEST(DecimalToString, WritesTheVenueFormat)
{
  EXPECT_EQ(Price::fromDouble(60000.0).toString(), "60000.000000");
  EXPECT_EQ(Price::fromDouble(57900.25).toString(), "57900.250000");
  EXPECT_EQ(Quantity::fromDouble(1.0).toString(), "1.000000");
  EXPECT_EQ(Price::fromDouble(0.08423).toString(), "0.084230");
  EXPECT_EQ(Price::fromDouble(-1.5).toString(), "-1.500000");
  EXPECT_EQ(Price::fromRaw(0).toString(), "0.000000");
}

TEST(DecimalToString, KeepsTheDecimalPointUnderACommaDecimalLocale)
{
  ScopedGlobalLocale loc;
  const std::string installed = loc.installCommaDecimal();
  if (installed.empty())
  {
    GTEST_SKIP() << "no comma-decimal locale on this host";
  }

  const std::string price = Price::fromDouble(60000.0).toString();
  EXPECT_EQ(price, "60000.000000") << "locale '" << installed << "'";
  EXPECT_EQ(price.find(','), std::string::npos)
      << "a comma decimal separator reached the order body under locale '" << installed << "'";
  EXPECT_EQ(Quantity::fromDouble(0.5).toString(), "0.500000") << "locale '" << installed << "'";
}

// The raw integer is the source of truth, and the eighth fractional digit it
// can carry is beyond what six digits print: the rounding must be the same
// one printf applied, not a truncation.
TEST(DecimalToString, RoundsTheSeventhDigitRatherThanTruncating)
{
  EXPECT_EQ(Price::fromRaw(150000).toString(), "0.001500");
  EXPECT_EQ(Price::fromRaw(199).toString(), "0.000002");
  EXPECT_EQ(Price::fromRaw(-199).toString(), "-0.000002");
  EXPECT_EQ(Price::fromRaw(99999999).toString(), "1.000000");
}
