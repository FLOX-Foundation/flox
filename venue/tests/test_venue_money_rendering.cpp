/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Two places where the venue hands a number to somebody else: the control
 * plane's instrument reply and the Prometheus page. Both spell a money value
 * with std::to_string(double), which is specified as printf("%f") and so has
 * two properties neither surface can afford.
 *
 * It renders six decimals. Every price and every rate in this venue is a raw
 * at a 1e-8 scale, so a one-tick instrument answers `"tick":0.000000` and a
 * 1e-7 funding rate is published as `0.000000` -- the two numbers an operator
 * would be reading the page for.
 *
 * And it follows the C locale. A process started with LC_NUMERIC set to a
 * locale that separates with a comma emits `0,000300`, which makes the control
 * reply invalid JSON and the metrics page unparseable by a scraper -- from a
 * setting that has nothing to do with the venue.
 *
 * Both surfaces are pinned here at the scale the money is kept in: eight
 * decimals, a dot, whatever the environment says.
 */
#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/messages.h"
#include "flox-venue/metrics.h"
#include "flox-venue/prometheus.h"
#include "flox-venue/symbol_config.h"

#include <gtest/gtest.h>

#include <clocale>
#include <cstdint>
#include <string>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;

// A locale whose decimal separator is a comma, for as long as this object is
// alive. Which one exists is a property of the host, so the candidates are
// tried in turn and the object reports whether any of them took.
class CommaLocale
{
 public:
  CommaLocale()
  {
    static constexpr const char* kCandidates[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "de_DE.utf8",
                                                  "fr_FR.utf8", "de_DE", "fr_FR"};
    for (const char* name : kCandidates)
    {
      if (std::setlocale(LC_ALL, name) == nullptr)
      {
        continue;
      }
      const lconv* c = std::localeconv();
      if (c != nullptr && c->decimal_point != nullptr && c->decimal_point[0] == ',')
      {
        name_ = name;
        return;
      }
    }
    std::setlocale(LC_ALL, "C");
  }

  ~CommaLocale() { std::setlocale(LC_ALL, "C"); }

  CommaLocale(const CommaLocale&) = delete;
  CommaLocale& operator=(const CommaLocale&) = delete;

  bool applied() const noexcept { return name_ != nullptr; }
  const char* name() const noexcept { return name_ == nullptr ? "(none)" : name_; }

 private:
  const char* name_{nullptr};
};

// A host with no German or French locale installed cannot exercise the
// separator half of these tests. The precision half does not depend on the
// locale and still runs, so the test stays a test either way -- it says so
// rather than passing quietly on half its subject.
void warnIfNoCommaLocale(const CommaLocale& locale)
{
  if (!locale.applied())
  {
    GTEST_LOG_(WARNING) << "no comma-decimal locale on this host: the separator was not exercised";
  }
}

// The value a Prometheus page publishes for `name`. The HELP and TYPE lines
// begin with '#', so the leading newline picks out the sample line.
std::string gaugeValue(const std::string& page, const std::string& name)
{
  const std::string key = "\n" + name + " ";
  const size_t at = page.find(key);
  if (at == std::string::npos)
  {
    return {};
  }
  const size_t from = at + key.size();
  const size_t to = page.find('\n', from);
  return page.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

// The value a control reply carries for `field`, read the way a JSON parser
// would end a number: at the separator or at the end of the object. A comma
// INSIDE the number therefore truncates it here exactly as it would there.
std::string jsonValue(const std::string& reply, const std::string& field)
{
  const std::string key = "\"" + field + "\":";
  const size_t at = reply.find(key);
  if (at == std::string::npos)
  {
    return {};
  }
  const size_t from = at + key.size();
  const size_t to = reply.find_first_of(",}", from);
  return reply.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

// Gauges carries the rate as a double today. A fix that carries the raw
// instead is the better shape and this test does not stand in its way.
template <class G>
void setFundingRate(G& g, int64_t raw)
{
  if constexpr (requires { g.fundingRateRaw; })
  {
    g.fundingRateRaw = raw;
  }
  else
  {
    g.fundingRate = static_cast<double>(raw) / static_cast<double>(kFundingRateScale);
  }
}

Gauges gauges(int64_t fundingRateRaw)
{
  Gauges g;
  g.insuranceFundRaw = static_cast<__int128>(5'000'000'000'000LL);
  g.openInterestRaw = static_cast<__int128>(120'000'000'000'000LL);
  g.openPositions = 7;
  g.restingOrders = 42;
  g.markPriceAgeNs = 250'000'000;
  g.liquidationsPaused = 1;
  setFundingRate(g, fundingRateRaw);
  return g;
}

// An instrument whose numbers need every decimal the scale has: a one-raw
// tick, and a band whose bounds are not round.
SymbolConfig instrument()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromRaw(1);                  // 0.00000001
  c.minPrice = Price::fromRaw(67'123'45678901LL);  // 67123.45678901
  c.maxPrice = Price::fromRaw(89'000'00000009LL);  // 89000.00000009
  return c;
}

std::string instrumentReply()
{
  InstrumentRegistry reg;
  reg.listInstrument(instrument());
  ControlApi api(reg);
  return api.handle(R"({"method":"get","symbol":1})");
}

}  // namespace

// The funding-rate gauge is a rate at kFundingRateScale, so it is published
// with the eight decimals that scale has. 0.03% and one raw are the two ends
// of what the venue has to be able to say.
TEST(VenueMoneyRendering, TheFundingRateGaugeCarriesEveryDecimalOfItsScale)
{
  const Metrics m;
  EXPECT_EQ(gaugeValue(prom::render(m, gauges(30'000)), "fme_funding_rate"), "0.00030000");
  EXPECT_EQ(gaugeValue(prom::render(m, gauges(1)), "fme_funding_rate"), "0.00000001");
  EXPECT_EQ(gaugeValue(prom::render(m, gauges(-30'000)), "fme_funding_rate"), "-0.00030000");
}

// And it is the same page whatever LC_NUMERIC the venue was started with. A
// comma here is not a cosmetic difference: `fme_funding_rate 0,000300` is a
// parse error for every scraper that reads the endpoint.
TEST(VenueMoneyRendering, TheMetricsPageIsRenderedWithADotInAnyLocale)
{
  const CommaLocale locale;
  const Metrics m;
  const std::string page = prom::render(m, gauges(30'000));
  const std::string value = gaugeValue(page, "fme_funding_rate");
  EXPECT_EQ(value, "0.00030000") << "locale " << locale.name();
  EXPECT_EQ(value.find(','), std::string::npos) << "locale " << locale.name();
  warnIfNoCommaLocale(locale);
}

// The control plane answers with the instrument's real numbers. A tick the
// reply rounds to zero is a tick an operator cannot confirm, and a band bound
// truncated to six decimals is a different band.
TEST(VenueMoneyRendering, TheInstrumentReplyCarriesEveryDecimalOfItsScale)
{
  const std::string reply = instrumentReply();
  EXPECT_EQ(jsonValue(reply, "tick"), "0.00000001") << reply;
  EXPECT_EQ(jsonValue(reply, "minPrice"), "67123.45678901") << reply;
  EXPECT_EQ(jsonValue(reply, "maxPrice"), "89000.00000009") << reply;
}

// And it is JSON whatever LC_NUMERIC says. Under a comma locale the number
// ends at the separator, so the reply the operator's tooling parses is a
// different document from the one the venue meant to send.
TEST(VenueMoneyRendering, TheInstrumentReplyIsJsonInAnyLocale)
{
  const CommaLocale locale;
  const std::string reply = instrumentReply();
  EXPECT_EQ(jsonValue(reply, "tick"), "0.00000001") << "locale " << locale.name() << ": " << reply;
  EXPECT_EQ(jsonValue(reply, "minPrice"), "67123.45678901")
      << "locale " << locale.name() << ": " << reply;
  warnIfNoCommaLocale(locale);
}

// The control. Everything on both surfaces that is already an integer or a
// keyword is locale-proof today and stays that way -- so a failure above is
// about the money rendering and not about the harness.
TEST(VenueMoneyRendering, TheIntegerFieldsAreAlreadyLocaleProof)
{
  const CommaLocale locale;
  const Metrics m;
  const std::string page = prom::render(m, gauges(30'000));
  EXPECT_EQ(gaugeValue(page, "fme_open_positions"), "7");
  EXPECT_EQ(gaugeValue(page, "fme_resting_orders"), "42");
  EXPECT_EQ(gaugeValue(page, "fme_insurance_fund_raw"), "5000000000000");

  const std::string reply = instrumentReply();
  EXPECT_EQ(jsonValue(reply, "symbol"), "1");
  EXPECT_EQ(jsonValue(reply, "halted"), "false");
  EXPECT_EQ(jsonValue(reply, "ok"), "true");
  warnIfNoCommaLocale(locale);
}
