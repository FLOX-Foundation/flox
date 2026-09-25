/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Last-look behaviour on the metrics page.
 *
 * The question an operator has to be able to answer from a scrape is not "how
 * many fills were refused" but "is this maker picking". A maker that refuses
 * only the fills that moved its way keeps the good ones and hands back the
 * bad: a free option, paid for by every taker. A maker refusing at a similar
 * rate in both directions is answering a latency problem instead. The totals
 * are identical in both cases, so a page that exports only totals cannot tell
 * an operator which one it is looking at.
 *
 * Every number here is read off an engine that actually ran the hold cycle --
 * never a hand-built struct. An exporter wired to a sampler nobody calls
 * renders zeros, and zeros look exactly like a healthy venue.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/prometheus.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t kHonest = 1;  // refuses in both directions
constexpr uint64_t kPicker = 2;  // refuses only when the move went its way

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.lastLookWindowNs = DurationNs{1000};
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

struct Cap
{
  std::vector<OutboundEvent> ev;
  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }
  const FillHeld* lastHeld() const
  {
    const FillHeld* last = nullptr;
    for (const auto& e : ev)
    {
      if (const auto* h = std::get_if<FillHeld>(&e))
      {
        last = h;
      }
    }
    return last;
  }
};

// One hold, resolved by the maker. `moveTo` is where the market goes during
// the window; `confirm` is the maker's answer.
struct Episode
{
  uint64_t maker;
  Side makerSide;
  double moveTo;
  bool confirm;
};

OrderId g_nextId = 100;

// A hold taken before anything has ever traded has no reference to measure the
// move against, and the engine classifies it as neither adverse nor
// favourable -- correctly, since nothing is known. Every episode below is
// therefore preceded by a print, which is also what a real venue looks like by
// the time last look matters.
void primeReference(MatchingEngine<MatchingBook>& eng, int64_t& ts)
{
  eng.submit(InboundCommand{limit(g_nextId++, Side::SELL, 100, 1, 5)}, ++ts);
  eng.submit(InboundCommand{limit(g_nextId++, Side::BUY, 100, 1, 6)}, ++ts);
}

void run(MatchingEngine<MatchingBook>& eng, Cap& cap, int64_t& ts, const Episode& ep)
{
  const Side takerSide = ep.makerSide == Side::SELL ? Side::BUY : Side::SELL;
  const OrderId makerId = g_nextId++;
  NewOrder mk = limit(makerId, ep.makerSide, 100, 1, ep.maker);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, ++ts);
  const OrderId takerId = g_nextId++;
  eng.submit(InboundCommand{limit(takerId, takerSide, 100, 1, 9)}, ++ts);
  const FillHeld* held = cap.lastHeld();
  ASSERT_NE(held, nullptr);
  const uint64_t heldId = held->heldId;
  // Move the market with a print, so the reference the window is measured
  // against actually moves.
  eng.submit(InboundCommand{limit(g_nextId++, Side::SELL, ep.moveTo, 1, 7)}, ++ts);
  eng.submit(InboundCommand{limit(g_nextId++, Side::BUY, ep.moveTo, 1, 8)}, ++ts);
  eng.submit(InboundCommand{LastLookDecision{heldId, SYM, ep.confirm, {}, ep.maker}}, ++ts);

  // A refused hold puts BOTH legs back on the book: the maker's quantity
  // returns to its price level, and the taker's residual rests per its TIF.
  // Left there, the next episode goes wrong in two different ways -- its taker
  // trades against the previous maker, or its maker crosses the previous
  // taker's resting bid and becomes the aggressor, so no hold is taken at all.
  // Both happened while writing this. The episode cleans up after itself.
  for (const auto& [id, account] :
       {std::pair<OrderId, uint64_t>{makerId, ep.maker}, std::pair<OrderId, uint64_t>{takerId, 9}})
  {
    CancelOrder c;
    c.id = id;
    c.symbol = SYM;
    c.accountId = account;
    eng.submit(InboundCommand{c}, ++ts);
  }
}

// One labeled series value out of a Prometheus page, or -1 when the series is
// absent. Absent and zero are different readings and the tests rely on it.
long long seriesValue(const std::string& page, const std::string& name, uint64_t maker)
{
  const std::string key = name + "{maker=\"" + std::to_string(maker) + "\"} ";
  const size_t at = page.find(key);
  if (at == std::string::npos)
  {
    return -1;
  }
  return std::stoll(page.substr(at + key.size()));
}

LastLookSample sample(const MatchingEngine<MatchingBook>& eng)
{
  LastLookSample l;
  l.byMaker = eng.lastLookStats();
  l.toleranceRejectedHolds = eng.toleranceRejectedHolds();
  l.skippedLastLookProRata = eng.skippedLastLookProRata();
  return l;
}

}  // namespace

// The whole point: two makers with the SAME refusal count, told apart by the
// direction of the moves they refused on.
TEST(LastLookMetrics, APickerAndAnHonestMakerAreDistinguishableInOneScrape)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  int64_t ts = 0;
  primeReference(eng, ts);

  // Honest: sold at 100, refuses whichever way the market went.
  run(eng, cap, ts, {kHonest, Side::SELL, 102.0, /*confirm=*/false});  // adverse, refused
  run(eng, cap, ts, {kHonest, Side::SELL, 98.0, /*confirm=*/false});   // favourable, refused

  // Picker: sold at 100, keeps the fill when the market rose against it only
  // if it must -- here it refuses exactly the one that went its way.
  run(eng, cap, ts, {kPicker, Side::SELL, 102.0, /*confirm=*/true});  // adverse, ACCEPTED
  run(eng, cap, ts, {kPicker, Side::SELL, 98.0, /*confirm=*/false});  // favourable, refused

  const std::string page = prom::render(Metrics{}, Gauges{}, sample(eng));

  EXPECT_EQ(seriesValue(page, "fme_last_look_holds_total", kHonest), 2);
  EXPECT_EQ(seriesValue(page, "fme_last_look_holds_total", kPicker), 2);

  // Totals alone: the honest maker refused twice, the picker once. Nothing
  // here says which one is worth a phone call.
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_total", kHonest), 2);
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_total", kPicker), 1);

  // The split does. The picker refused ONLY on a favourable move; the honest
  // maker refused on both.
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_favourable_total", kPicker), 1);
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_adverse_total", kPicker), 0);
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_favourable_total", kHonest), 1);
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_adverse_total", kHonest), 1);
}

// A maker that behaves must still appear. A series that vanishes when its
// value is zero cannot be alerted on, and "no series" would be read as "no
// data" rather than "held ten, refused none".
TEST(LastLookMetrics, AMakerThatRefusedNothingStillHasSeries)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  int64_t ts = 0;
  primeReference(eng, ts);
  run(eng, cap, ts, {kHonest, Side::SELL, 102.0, /*confirm=*/true});
  run(eng, cap, ts, {kHonest, Side::SELL, 98.0, /*confirm=*/true});

  const std::string page = prom::render(Metrics{}, Gauges{}, sample(eng));
  EXPECT_EQ(seriesValue(page, "fme_last_look_holds_total", kHonest), 2);
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_total", kHonest), 0)
      << "present and zero, not absent";
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_adverse_total", kHonest), 0);
  EXPECT_EQ(seriesValue(page, "fme_last_look_rejects_favourable_total", kHonest), 0);
}

// The venue's own tolerance refuses holds too, and blaming a maker for those
// would be wrong -- they are counted venue-wide and exported separately.
TEST(LastLookMetrics, TheVenuesOwnRefusalsAreCountedApartFromTheMakers)
{
  Cap cap;
  SymbolConfig c = cfg();
  c.lastLookToleranceRaw = px(0.5).raw();  // a move beyond this is refused by the venue
  MatchingEngine<MatchingBook> eng(c, cap.sink());
  int64_t ts = 0;
  primeReference(eng, ts);
  // The maker says yes; the market moved far more than the tolerance allows.
  run(eng, cap, ts, {kHonest, Side::SELL, 105.0, /*confirm=*/true});

  const LastLookSample l = sample(eng);
  EXPECT_GT(l.toleranceRejectedHolds, 0u) << "the venue refused it, whatever the maker answered";

  const std::string page = prom::render(Metrics{}, Gauges{}, l);
  // From the start of a line: the same name appears first in the HELP text,
  // where what follows it is prose rather than a number.
  const std::string key = "\nfme_last_look_tolerance_rejects_total ";
  const size_t at = page.find(key);
  ASSERT_NE(at, std::string::npos);
  EXPECT_EQ(std::stoull(page.substr(at + key.size())), l.toleranceRejectedHolds);
}

// Every series the exporter offers must be reachable from a live engine. A
// sampler that forgets one renders a zero that reads as healthy, which is the
// failure this whole file exists to prevent.
TEST(LastLookMetrics, EverySeriesTheExporterOffersIsPresentOnALiveRun)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  int64_t ts = 0;
  primeReference(eng, ts);
  run(eng, cap, ts, {kHonest, Side::SELL, 102.0, /*confirm=*/false});

  const std::string page = prom::render(Metrics{}, Gauges{}, sample(eng));

  // Per-maker series: a TYPE line and at least one labeled sample.
  for (const char* name : {"fme_last_look_holds_total", "fme_last_look_rejects_total",
                           "fme_last_look_rejects_adverse_total",
                           "fme_last_look_rejects_favourable_total"})
  {
    EXPECT_NE(page.find(std::string("# TYPE ") + name), std::string::npos)
        << name << " has no TYPE line";
    EXPECT_NE(page.find(std::string(name) + "{maker="), std::string::npos)
        << name << " has a TYPE line and no sample: the series is declared and never filled";
  }

  // Venue-wide series carry no label, so they are looked for by the bare name
  // followed by a value.
  for (const char* name :
       {"fme_last_look_tolerance_rejects_total", "fme_last_look_prorata_skips_total"})
  {
    EXPECT_NE(page.find(std::string("# TYPE ") + name), std::string::npos)
        << name << " has no TYPE line";
    EXPECT_NE(page.find(std::string("\n") + name + " "), std::string::npos)
        << name << " has a TYPE line and no sample";
  }
}
