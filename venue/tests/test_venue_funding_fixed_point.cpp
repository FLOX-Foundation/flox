/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Funding is a settlement, and a settlement is fixed-point.
 *
 * ApplyFunding carries a `double rate` in a JOURNALED body, and Clearing
 * spends it twice as a double: once to derive the rate raw it publishes and
 * hashes (`rate * kFundingRateScale`, truncated), and once per open position to
 * size the payment (`static_cast<Amount>(static_cast<double>(notional) *
 * rate)`). Neither is the rate the operator sent.
 *
 * 0.0003 is the whole demonstration. The nearest double to it is
 * 0.00029999999999999997372, so the venue publishes a rate of 0.00029999 where
 * an operator set 0.03%, and a long paying funding on a 200,000-unit position
 * is charged one raw less than it owes -- every interval, from a rate that
 * round-trips through the journal bit for bit. Both numbers below are exact
 * integers in fixed point (the notional divides the rate's denominator), so no
 * rounding rule reaches the answers the double path returns.
 */
#include "flox-venue/cross_margin.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 999;
constexpr int64_t SEC = 1'000'000'000LL;

// The position: 4 units long at a mark of 50000, i.e. 200,000 quote units.
constexpr int64_t kMarkRaw = 50'000'00000000LL;
constexpr int64_t kQtyRaw = 4'00000000LL;
constexpr int64_t kNotionalRaw = 20'000'000'000'000LL;

// 0.03% per interval. The rate raw the journalled body means, and the payment
// it sizes: 20000000000000 * 30000 / 100000000 = 6000000000 exactly, i.e.
// 60.00000000 quote units.
constexpr double kRate = 0.0003;
constexpr int64_t kRateRaw = 30'000;
constexpr int64_t kPaymentRaw = 6'000'000'000LL;

// The control rate. 0.0001 already survives the double round trip, and has to
// keep surviving it.
constexpr double kControlRate = 0.0001;
constexpr int64_t kControlRateRaw = 10'000;

SymbolConfig perpCfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromRaw(1);
  c.minPrice = Price::fromRaw(1);
  c.maxPrice = Price::fromRaw(1'000'000'00000000LL);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.maintenanceMarginBps = 0;  // liquidation off: this file is about the payment
  c.fundingIntervalNs = DurationNs{8 * SEC};
  return c;
}

AdjustPosition seedPosition(uint64_t acct, int64_t qtyDeltaRaw, int64_t entryRaw)
{
  AdjustPosition a{};
  a.accountId = acct;
  a.symbol = SYM;
  a.qtyDeltaRaw = qtyDeltaRaw;
  a.entryRaw = entryRaw;
  a.reason = AdjustReason::Reconciliation;
  return a;
}

// An engine with a ledger, a seeded long and a mark, ready to be funded.
struct Eng
{
  std::vector<OutboundEvent> out;
  Ledger led;
  MatchingEngine<MatchingBook> eng;

  static constexpr Amount kSeed = static_cast<Amount>(100'000'000'000'000LL);

  Eng()
      : eng(perpCfg(), [this](const OutboundEvent& e)
            { out.push_back(e); })
  {
    eng.setLedger(&led, VENUE_ACCT);
    led.deposit(1, QUOTE, kSeed);
    led.deposit(VENUE_ACCT, QUOTE, kSeed);
    eng.submit(InboundCommand{seedPosition(1, kQtyRaw, kMarkRaw)}, 1 * SEC);
    eng.submit(InboundCommand{SetMark{SYM, {}, Price::fromRaw(kMarkRaw)}}, 2 * SEC);
  }

  const DerivativesUpdated* lastDerivatives() const
  {
    const DerivativesUpdated* found = nullptr;
    for (const OutboundEvent& e : out)
    {
      if (const auto* d = std::get_if<DerivativesUpdated>(&e))
      {
        found = d;
      }
    }
    return found;
  }
};

InboundCommand funding(double rate)
{
  return InboundCommand{ApplyFunding{SYM, {}, rate, Price::fromRaw(kMarkRaw)}};
}

}  // namespace

// The arithmetic the expectations stand on: the notional is a whole multiple
// of the rate's denominator, so the payment is an exact integer of raws and no
// rounding convention is being asserted here.
TEST(VenueFundingFixedPoint, TheExpectedRawsAreNotAChoiceOfRoundingRule)
{
  const __int128 notional = static_cast<__int128>(kMarkRaw) * kQtyRaw / Price::Scale;
  EXPECT_EQ(static_cast<int64_t>(notional), kNotionalRaw);

  const __int128 num = notional * kRateRaw;
  EXPECT_EQ(static_cast<int64_t>(num % kFundingRateScale), 0);
  EXPECT_EQ(static_cast<int64_t>(num / kFundingRateScale), kPaymentRaw);
}

// The rate the venue publishes and hashes is the rate the operator sent.
TEST(VenueFundingFixedPoint, ThePublishedRateIsTheRateThatWasSent)
{
  Eng e;
  e.eng.submit(funding(kRate), 3 * SEC);

  EXPECT_EQ(e.eng.fundingRateRaw(), kRateRaw);
  ASSERT_NE(e.lastDerivatives(), nullptr);
  EXPECT_EQ(e.lastDerivatives()->fundingRateRaw, kRateRaw);
}

// The payment is the exact fixed-point share of the notional.
TEST(VenueFundingFixedPoint, ALongPaysTheExactRaw)
{
  Eng e;
  e.eng.submit(funding(kRate), 3 * SEC);

  EXPECT_EQ(e.led.available(1, QUOTE), Eng::kSeed - kPaymentRaw);
  EXPECT_EQ(e.led.available(VENUE_ACCT, QUOTE), Eng::kSeed + kPaymentRaw);
}

// Funding is periodic, so a per-interval error is an accruing one: a thousand
// intervals of the same rate on the same position is a thousand raws off a
// balance the venue reconciles against.
TEST(VenueFundingFixedPoint, AThousandIntervalsSumToTheExactRaw)
{
  constexpr int kIntervals = 1000;
  Eng e;
  for (int i = 0; i < kIntervals; ++i)
  {
    e.eng.submit(funding(kRate), (3 + i) * SEC);
  }

  const Amount total = static_cast<Amount>(kPaymentRaw) * kIntervals;
  EXPECT_EQ(e.led.available(1, QUOTE), Eng::kSeed - total);
  EXPECT_EQ(e.led.available(VENUE_ACCT, QUOTE), Eng::kSeed + total);
}

// The journalled body is what a recovering venue reads, so the rate has to
// mean the same thing on the way back in: the same raw, and the same payment,
// after a write and a load.
TEST(VenueFundingFixedPoint, TheJournalledRateReplaysToTheSameRaw)
{
  const std::string path = tmpPath("venue_funding_rate", ".jrn");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(funding(kRate), 3 * SEC);
    j.flush();
  }

  const auto records = Journal::loadTimed(path);
  std::remove(path.c_str());
  ASSERT_EQ(records.size(), 1U);
  const auto* back = std::get_if<ApplyFunding>(&records[0].second);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->rate, kRate);  // the body itself is blittable and exact

  Eng e;
  e.eng.submit(records[0].second, records[0].first);
  EXPECT_EQ(e.eng.fundingRateRaw(), kRateRaw);
  EXPECT_EQ(e.led.available(1, QUOTE), Eng::kSeed - kPaymentRaw);
}

// The control: 0.0001 already means 0.0001 and must go on meaning it, live and
// after a journal round trip.
TEST(VenueFundingFixedPoint, AControlRateAlreadyRoundTripsAndStaysThatWay)
{
  Eng live;
  live.eng.submit(funding(kControlRate), 3 * SEC);
  EXPECT_EQ(live.eng.fundingRateRaw(), kControlRateRaw);

  const std::string path = tmpPath("venue_funding_control", ".jrn");
  std::remove(path.c_str());
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(funding(kControlRate), 3 * SEC);
    j.flush();
  }
  const auto records = Journal::loadTimed(path);
  std::remove(path.c_str());
  ASSERT_EQ(records.size(), 1U);

  Eng replayed;
  replayed.eng.submit(records[0].second, records[0].first);
  EXPECT_EQ(replayed.eng.fundingRateRaw(), kControlRateRaw);
  EXPECT_EQ(replayed.eng.fundingRateRaw(), live.eng.fundingRateRaw());
}

// The portfolio-margin book funds through its own copy of the same expression
// (cross_margin.h), so it carries the same error. Longs pay shorts, the venue
// pool mirrors both legs, and each leg is the exact raw.
TEST(VenueFundingFixedPoint, ThePortfolioBookPaysTheExactRawToo)
{
  Ledger led;
  const Amount seed = static_cast<Amount>(100'000'000'000'000LL);
  led.deposit(1, QUOTE, seed);
  led.deposit(2, QUOTE, seed);

  CrossMarginManager cm(led, QUOTE, VENUE_ACCT);
  cm.configureSymbol(SYM, /*imBps*/ 1000, /*mmBps*/ 0);
  cm.setMark(SYM, Price::fromRaw(kMarkRaw));
  cm.applyFill(1, SYM, Side::BUY, kQtyRaw, kMarkRaw);
  cm.applyFill(2, SYM, Side::SELL, kQtyRaw, kMarkRaw);

  const Amount longBefore = led.total(1, QUOTE);
  const Amount shortBefore = led.total(2, QUOTE);

  cm.applyFunding(SYM, kRate, Price::fromRaw(kMarkRaw));

  EXPECT_EQ(led.total(1, QUOTE), longBefore - kPaymentRaw);
  EXPECT_EQ(led.total(2, QUOTE), shortBefore + kPaymentRaw);
  EXPECT_EQ(led.total(VENUE_ACCT, QUOTE), 0);  // zero-sum across a balanced book
}

// ---- the two edges of the rate boundary ----------------------------------

// A notional wider than an int64 raw. venue::Amount is a 128-bit integer
// precisely so a notional cannot overflow, and the rate has to be applied at
// that width rather than to a notional squeezed into an int64 on the way in:
// a clamp there does not fail, it settles a different, smaller number.
//
// 100 units at a mark of 1e10 is 1e20 quote raws -- 1e12 quote units, which no
// venue has seen and the type nonetheless carries.
TEST(VenueFundingFixedPoint, ANotionalWiderThanAnInt64IsSettledAtFullWidth)
{
  const Amount wide = static_cast<Amount>(100'000'000'000LL) * 1'000'000'000LL;  // 1e20
  ASSERT_GT(wide, static_cast<Amount>((std::numeric_limits<int64_t>::max)()));

  // The arithmetic directly: 1e20 at 0.03% is 3e16, and the extra 400000 raw
  // of notional is another 120 -- a real multiply and divide, not a clamp.
  EXPECT_EQ(rateOnNotional(wide, kRateRaw, kFundingRateScale),
            static_cast<Amount>(30'000'000'000'000'000LL));
  EXPECT_EQ(rateOnNotional(wide + 400'000, kRateRaw, kFundingRateScale),
            static_cast<Amount>(30'000'000'000'000'120LL));
  EXPECT_EQ(rateOnNotional(-wide, kRateRaw, kFundingRateScale),
            -static_cast<Amount>(30'000'000'000'000'000LL));

  // And through the settlement path, where the payment lands in the ledger.
  constexpr int64_t kWideMarkRaw = 1'000'000'000'000'000'000LL;  // 1e10 per unit
  constexpr int64_t kWideQtyRaw = 10'000'000'000LL;              // 100 units
  SymbolConfig c = perpCfg();
  c.maxPrice = Price::fromRaw(9'000'000'000'000'000'000LL);

  std::vector<OutboundEvent> out;
  Ledger led;
  MatchingEngine<MatchingBook> eng(c, [&out](const OutboundEvent& e)
                                   { out.push_back(e); });
  eng.setLedger(&led, VENUE_ACCT);
  eng.submit(InboundCommand{seedPosition(1, kWideQtyRaw, kWideMarkRaw)}, 1 * SEC);
  eng.submit(InboundCommand{SetMark{SYM, {}, Price::fromRaw(kWideMarkRaw)}}, 2 * SEC);
  eng.submit(InboundCommand{ApplyFunding{SYM, {}, kRate, Price::fromRaw(kWideMarkRaw)}}, 3 * SEC);

  EXPECT_EQ(led.available(1, QUOTE), -static_cast<Amount>(30'000'000'000'000'000LL));
  EXPECT_EQ(led.available(VENUE_ACCT, QUOTE), static_cast<Amount>(30'000'000'000'000'000LL));
}

// The rate crosses into fixed point ONCE, and what is settled is the raw that
// was published. Below about 4.5e15 raw a raw can be sent back through a
// double and come home unchanged, so a second conversion hides; above it the
// double no longer holds every raw and the two answers part. This rate is
// absurd as a rate -- four billion percent an interval -- and that is the
// point: it is the smallest magnitude at which "published" and "settled" can
// be told apart at all, and they have to be the same number there too.
TEST(VenueFundingFixedPoint, TheSettledRateIsTheRateThatWasPublished)
{
  constexpr double kHugeRate = 40000000.000000186265;
  constexpr int64_t kHugeRateRaw = 4'000'000'000'000'019LL;

  // A small position, so the payment still fits the int64 the fee side
  // narrows to: 4 units at a mark of 250 is 1000 quote units of notional.
  constexpr int64_t kSmallMarkRaw = 250'00000000LL;
  constexpr int64_t kSmallNotionalRaw = 100'000'000'000LL;
  constexpr int64_t kHugePaymentRaw = 4'000'000'000'000'019'000LL;

  const __int128 num = static_cast<__int128>(kSmallNotionalRaw) * kHugeRateRaw;
  ASSERT_EQ(static_cast<int64_t>(num % kFundingRateScale), 0);
  ASSERT_EQ(static_cast<int64_t>(num / kFundingRateScale), kHugePaymentRaw);

  std::vector<OutboundEvent> out;
  Ledger led;
  MatchingEngine<MatchingBook> eng(perpCfg(), [&out](const OutboundEvent& e)
                                   { out.push_back(e); });
  eng.setLedger(&led, VENUE_ACCT);
  eng.submit(InboundCommand{seedPosition(1, kQtyRaw, kSmallMarkRaw)}, 1 * SEC);
  eng.submit(InboundCommand{SetMark{SYM, {}, Price::fromRaw(kSmallMarkRaw)}}, 2 * SEC);
  eng.submit(InboundCommand{ApplyFunding{SYM, {}, kHugeRate, Price::fromRaw(kSmallMarkRaw)}},
             3 * SEC);

  EXPECT_EQ(eng.fundingRateRaw(), kHugeRateRaw);
  EXPECT_EQ(led.available(1, QUOTE), -static_cast<Amount>(kHugePaymentRaw));
  // Said once more as the contract itself: the payment is the PUBLISHED raw
  // applied to the notional, with nothing converted in between.
  EXPECT_EQ(led.available(1, QUOTE),
            -rateOnNotional(kSmallNotionalRaw, eng.fundingRateRaw(), kFundingRateScale));
}

// A rate that is not a number. ApplyFunding carries a double, which is what a
// rate calculator produces, and a calculator that divided by an index it did
// not have produces this one. It is journaled, so it replays; it is published,
// so a client reads it; and it is spent against every open position. Zero is
// the only raw it can become -- the venue publishes no rate and settles
// nothing, rather than transferring a number nobody computed.
TEST(VenueFundingFixedPoint, ANaNRatePublishesZeroAndSettlesNothing)
{
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  ASSERT_NE(kNaN, kNaN);

  EXPECT_EQ(fundingRateRawOf(kNaN), 0);

  Eng e;
  const Amount before = e.led.available(1, QUOTE);
  e.eng.submit(funding(kNaN), 3 * SEC);

  EXPECT_EQ(e.eng.fundingRateRaw(), 0);
  ASSERT_NE(e.lastDerivatives(), nullptr);
  EXPECT_EQ(e.lastDerivatives()->fundingRateRaw, 0);
  EXPECT_EQ(e.led.available(1, QUOTE), before);
  EXPECT_EQ(e.led.available(VENUE_ACCT, QUOTE), Eng::kSeed);

  // The portfolio book answers the same way, from its own copy of the
  // boundary.
  Ledger led;
  const Amount seed = static_cast<Amount>(100'000'000'000'000LL);
  led.deposit(1, QUOTE, seed);
  led.deposit(2, QUOTE, seed);
  CrossMarginManager cm(led, QUOTE, VENUE_ACCT);
  cm.configureSymbol(SYM, /*imBps*/ 1000, /*mmBps*/ 0);
  cm.setMark(SYM, Price::fromRaw(kMarkRaw));
  cm.applyFill(1, SYM, Side::BUY, kQtyRaw, kMarkRaw);
  cm.applyFill(2, SYM, Side::SELL, kQtyRaw, kMarkRaw);
  const Amount longBefore = led.total(1, QUOTE);
  const Amount shortBefore = led.total(2, QUOTE);

  cm.applyFunding(SYM, kNaN, Price::fromRaw(kMarkRaw));

  EXPECT_EQ(led.total(1, QUOTE), longBefore);
  EXPECT_EQ(led.total(2, QUOTE), shortBefore);
}
