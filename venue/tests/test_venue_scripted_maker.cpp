/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A maker that behaves on command, and the metric that catches the one
 * behaviour worth catching.
 *
 * Five scenarios, each driven through a real engine: the decisions are real
 * LastLookDecision commands applied through the path live traffic uses, not a
 * mock's return value. Someone writing a client gets to develop against a
 * counterparty that accepts everything, refuses everything, refuses only when
 * hurt, refuses only when it profits, or says nothing at all.
 *
 * The fourth of those is the point. A maker refusing only the fills that
 * moved its way is taking a free option, and its REJECT TOTAL is identical to
 * an honest maker's -- so this file is also where the metric
 * gets its own verification: a scripted picker must be visible in
 * fme_last_look_rejects_favourable_total and invisible in the total.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/prometheus.h"
#include "flox-venue/script/quote_generators.h"
#include "flox-venue/script/scripted_maker.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using namespace flox::venue::script;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t kMaker = 1;
constexpr uint64_t kTaker = 9;
constexpr uint64_t kMover = 5;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.lastLookWindowNs = DurationNs{1'000'000};
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

// One engine, one scripted maker, and a driver that moves the market between
// the hold and the decision -- which is the only way the directional policies
// mean anything.
struct Harness
{
  std::vector<OutboundEvent> ev;
  ScriptedMaker maker;
  MatchingEngine<MatchingBook> eng;
  int64_t ts{0};
  OrderId nextId{100};

  explicit Harness(MakerPolicy policy)
      : maker(kMaker, policy),
        eng(cfg(), [this](const OutboundEvent& e)
            {
              ev.push_back(e);
              maker.observe(e); })
  {
    maker.setSymbol(SYM);
  }

  void submit(const InboundCommand& c) { eng.submit(c, ++ts); }

  // A print, so the maker has a reference before any hold is taken.
  void prime(double price)
  {
    submit(InboundCommand{limit(nextId++, Side::SELL, price, 1, kMover)});
    submit(InboundCommand{limit(nextId++, Side::BUY, price, 1, kMover)});
  }

  // One episode: the maker quotes, a taker hits it, the market moves, the
  // maker answers.
  void episode(Side makerSide, double quote, double moveTo)
  {
    const OrderId makerId = nextId++;
    NewOrder mk = limit(makerId, makerSide, quote, 1, kMaker);
    mk.lastLook = true;
    submit(InboundCommand{mk});

    const Side takerSide = makerSide == Side::SELL ? Side::BUY : Side::SELL;
    submit(InboundCommand{limit(nextId++, takerSide, quote, 1, kTaker)});

    // The move, during the window.
    submit(InboundCommand{limit(nextId++, Side::SELL, moveTo, 1, kMover)});
    submit(InboundCommand{limit(nextId++, Side::BUY, moveTo, 1, kMover)});

    std::vector<InboundCommand> decisions;
    maker.decide(decisions);
    for (const auto& d : decisions)
    {
      submit(d);
    }

    // Leave nothing behind: a refused hold returns both legs to the book, and
    // the next episode's taker would trade against them instead.
    for (OrderId id : {makerId, static_cast<OrderId>(nextId - 3)})
    {
      CancelOrder c;
      c.id = id;
      c.symbol = SYM;
      c.accountId = (id == makerId) ? kMaker : kTaker;
      submit(InboundCommand{c});
    }
  }

  int count(bool (*pred)(const OutboundEvent&)) const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      n += pred(e) ? 1 : 0;
    }
    return n;
  }
};

bool isTrade(const OutboundEvent& e) { return std::get_if<Trade>(&e) != nullptr; }
bool isFillRejected(const OutboundEvent& e) { return std::get_if<FillRejected>(&e) != nullptr; }

LastLookSample sample(const MatchingEngine<MatchingBook>& eng)
{
  LastLookSample l;
  l.byMaker = eng.lastLookStats();
  l.toleranceRejectedHolds = eng.toleranceRejectedHolds();
  l.skippedLastLookProRata = eng.skippedLastLookProRata();
  return l;
}

long long series(const std::string& page, const std::string& name, uint64_t maker)
{
  const std::string key = name + "{maker=\"" + std::to_string(maker) + "\"} ";
  const size_t at = page.find(key);
  return at == std::string::npos ? -1 : std::stoll(page.substr(at + key.size()));
}

}  // namespace

TEST(ScriptedMaker, AcceptAlwaysPrintsEveryHeldFill)
{
  Harness h(MakerPolicy::AcceptAlways);
  h.prime(100.0);
  h.episode(Side::SELL, 100.0, 102.0);  // adverse
  h.episode(Side::SELL, 100.0, 98.0);   // favourable

  EXPECT_EQ(h.maker.decisions(), 2u);
  EXPECT_EQ(h.maker.accepts(), 2u);
  EXPECT_EQ(h.count(isFillRejected), 0) << "it accepted everything and something was refused";
}

TEST(ScriptedMaker, RejectAlwaysPrintsNoneOfThem)
{
  Harness h(MakerPolicy::RejectAlways);
  h.prime(100.0);
  h.episode(Side::SELL, 100.0, 102.0);
  h.episode(Side::SELL, 100.0, 98.0);

  EXPECT_EQ(h.maker.decisions(), 2u);
  EXPECT_EQ(h.maker.accepts(), 0u);
  EXPECT_EQ(h.count(isFillRejected), 2) << "it refused everything and something still printed";
}

// The honest one: it refuses when the market left its price behind, and
// honours the rest.
TEST(ScriptedMaker, RejectOnAdverseRefusesOnlyWhenTheMoveWentAgainstIt)
{
  Harness h(MakerPolicy::RejectOnAdverse);
  h.prime(100.0);
  h.episode(Side::SELL, 100.0, 102.0);  // sold at 100, market at 102: hurt
  h.episode(Side::SELL, 100.0, 98.0);   // sold at 100, market at 98: fine

  EXPECT_EQ(h.maker.decisions(), 2u);
  EXPECT_EQ(h.maker.accepts(), 1u) << "it should have honoured exactly the one that went its way";

  const std::string page = prom::render(Metrics{}, Gauges{}, sample(h.eng));
  EXPECT_EQ(series(page, "fme_last_look_rejects_adverse_total", kMaker), 1);
  EXPECT_EQ(series(page, "fme_last_look_rejects_favourable_total", kMaker), 0);
}

// The one the venue wants to see. Same reject count as the honest maker
// above; only the direction tells them apart, which is the whole reason the
// metric is split.
TEST(ScriptedMaker, APickerIsInvisibleInTheTotalAndObviousInTheSplit)
{
  Harness honest(MakerPolicy::RejectOnAdverse);
  honest.prime(100.0);
  honest.episode(Side::SELL, 100.0, 102.0);
  honest.episode(Side::SELL, 100.0, 98.0);

  Harness picker(MakerPolicy::RejectOnFavourable);
  picker.prime(100.0);
  picker.episode(Side::SELL, 100.0, 102.0);
  picker.episode(Side::SELL, 100.0, 98.0);

  const std::string honestPage = prom::render(Metrics{}, Gauges{}, sample(honest.eng));
  const std::string pickerPage = prom::render(Metrics{}, Gauges{}, sample(picker.eng));

  // Identical by the total. An alert on this number cannot tell them apart.
  EXPECT_EQ(series(honestPage, "fme_last_look_rejects_total", kMaker),
            series(pickerPage, "fme_last_look_rejects_total", kMaker));

  // Opposite by direction, which is what makes the picker visible.
  EXPECT_EQ(series(honestPage, "fme_last_look_rejects_adverse_total", kMaker), 1);
  EXPECT_EQ(series(honestPage, "fme_last_look_rejects_favourable_total", kMaker), 0);
  EXPECT_EQ(series(pickerPage, "fme_last_look_rejects_adverse_total", kMaker), 0);
  EXPECT_EQ(series(pickerPage, "fme_last_look_rejects_favourable_total", kMaker), 1);
}

// Says nothing at all. The venue's window is what resolves the hold, not the
// maker -- a client has to handle a counterparty that simply goes quiet.
TEST(ScriptedMaker, SilentAnswersNothingAndLeavesTheVenueToTimeItOut)
{
  Harness h(MakerPolicy::Silent);
  h.prime(100.0);
  h.episode(Side::SELL, 100.0, 102.0);

  EXPECT_EQ(h.maker.decisions(), 0u) << "a silent maker sent a decision";
  // The hold is still the venue's to resolve: nothing was printed for it and
  // nothing was refused by the maker.
  EXPECT_EQ(h.eng.lastLookStats().at(kMaker).held, 1u);
}

// A scripted maker answers for its own holds and nobody else's. With one
// maker in the venue there are no foreign holds to ignore, so the filter
// looks right and is untested -- a mutation that removes it passed green
// until this existed. In a venue with two last-look makers the harness would
// otherwise answer for a counterparty it does not own, the venue would refuse
// it as NotOrderOwner, and the harness would report decisions it never got to
// make.
TEST(ScriptedMaker, ItAnswersOnlyForItsOwnHolds)
{
  constexpr uint64_t kOtherMaker = 2;

  std::vector<OutboundEvent> ev;
  ScriptedMaker mine(kMaker, MakerPolicy::RejectAlways);
  mine.setSymbol(SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     ev.push_back(e);
                                     mine.observe(e); });
  int64_t ts = 0;
  OrderId id = 300;
  const auto submit = [&](const InboundCommand& c)
  { eng.submit(c, ++ts); };

  // A print, so a reference exists.
  submit(InboundCommand{limit(id++, Side::SELL, 100.0, 1, kMover)});
  submit(InboundCommand{limit(id++, Side::BUY, 100.0, 1, kMover)});

  // The OTHER maker is hit and held. Ours never quoted at all.
  const OrderId theirs = id++;
  NewOrder mk = limit(theirs, Side::SELL, 100.0, 1, kOtherMaker);
  mk.lastLook = true;
  submit(InboundCommand{mk});
  submit(InboundCommand{limit(id++, Side::BUY, 100.0, 1, kTaker)});

  bool sawHold = false;
  for (const auto& e : ev)
  {
    if (const auto* h = std::get_if<FillHeld>(&e))
    {
      sawHold = sawHold || h->makerAccount == kOtherMaker;
    }
  }
  ASSERT_TRUE(sawHold) << "the other maker was never held, so there is nothing to ignore";

  std::vector<InboundCommand> decisions;
  mine.decide(decisions);
  EXPECT_TRUE(decisions.empty()) << "it answered for a hold belonging to another account";
  EXPECT_EQ(mine.decisions(), 0u);
  EXPECT_EQ(mine.openHolds(), 0u);
}

// ── generated flow ───────────────────────────────────────────────────

namespace
{
uint64_t digest(const std::vector<InboundCommand>& cmds)
{
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v)
  {
    h ^= v;
    h *= 1099511628211ull;
  };
  for (const auto& c : cmds)
  {
    const auto* o = std::get_if<Quote>(&c);
    if (o == nullptr)
    {
      continue;
    }
    mix(o->bidId);
    mix(o->askId);
    mix(static_cast<uint64_t>(o->bidPrice.raw()));
    mix(static_cast<uint64_t>(o->askPrice.raw()));
    mix(static_cast<uint64_t>(o->bidQty.raw()));
  }
  return h;
}
}  // namespace

// The only thing a generator has to promise: the same seed is the same flow.
// Without it a client's failure is a story about what the market happened to
// be doing, and nobody can go back to it.
TEST(GeneratedFlow, TheSameSeedProducesTheSameCommands)
{
  LadderQuoter::Config cfg;
  cfg.symbol = SYM;
  cfg.levels = 5;

  std::vector<InboundCommand> a;
  std::vector<InboundCommand> b;
  LadderQuoter q1(cfg, 12345);
  LadderQuoter q2(cfg, 12345);
  for (int i = 0; i < 200; ++i)
  {
    q1.next(a);
    q2.next(b);
  }
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(digest(a), digest(b));
  EXPECT_EQ(q1.midRaw(), q2.midRaw()) << "the mid walked differently on the same seed";
}

TEST(GeneratedFlow, ADifferentSeedProducesDifferentFlow)
{
  LadderQuoter::Config cfg;
  cfg.symbol = SYM;
  std::vector<InboundCommand> a;
  std::vector<InboundCommand> b;
  LadderQuoter q1(cfg, 1);
  LadderQuoter q2(cfg, 2);
  for (int i = 0; i < 200; ++i)
  {
    q1.next(a);
    q2.next(b);
  }
  EXPECT_NE(digest(a), digest(b)) << "two seeds produced identical flow: the seed does nothing";
}

// A round must REPLACE the previous quotes, and the only witness that counts
// is the venue: the first version of this test compared the ids the generator
// produced and passed while every round after the first came back
// DuplicateOrderId. Ask the engine instead.
TEST(GeneratedFlow, TheVenueAcceptsEveryRoundRatherThanRefusingRepeats)
{
  venue::SymbolConfig sc = cfg();
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(sc, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });

  LadderQuoter::Config lc;
  lc.symbol = SYM;
  lc.levels = 3;
  lc.midRaw = px(100.0).raw();
  lc.tickRaw = px(0.01).raw();
  LadderQuoter q(lc, 77);

  std::vector<InboundCommand> out;
  int64_t ts = 0;
  for (int round = 0; round < 5; ++round)
  {
    out.clear();
    q.next(out);
    EXPECT_EQ(static_cast<int>(out.size()), q.perRound());
    for (const auto& c : out)
    {
      eng.submit(c, ++ts);
    }
  }

  int rejects = 0;
  int accepted = 0;
  for (const auto& e : ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      ++rejects;
      EXPECT_NE(r->reason, RejectReason::DuplicateOrderId)
          << "a round was refused as a repeat: the ladder is not being replaced";
    }
    accepted += std::get_if<OrderAccepted>(&e) != nullptr ? 1 : 0;
  }
  EXPECT_EQ(rejects, 0);
  EXPECT_GT(accepted, q.perRound()) << "only the first round ever reached the book";
}
