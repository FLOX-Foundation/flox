/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Can a pro-rata venue actually be run?
 *
 * docs/venue/matching.md presents pro-rata as one of the two first-class
 * allocation rules, and the matcher implements it. What carries it is a
 * constructor argument of MatchingEngine and nothing else: SequencedShard and
 * SymbolRouter both build their engine from a SymbolConfig and take the
 * default. So the policy is reachable from a test that constructs an engine by
 * hand and from nowhere a deployment can go -- no journal, no recovery, no
 * checkpoint, no gateway.
 *
 * The configuration is the only route into either of those two, so the policy
 * has to travel on it. These tests are written against
 * `SymbolConfig::matchPolicy`; the engine's constructor argument keeps working
 * for the callers that already pass it.
 *
 * needs: MatchPolicy SymbolConfig::matchPolicy{MatchPolicy::PriceTimeFifo};
 * needs: SequencedShard and SymbolRouter<Book>::addSymbol build their engine
 *        with that policy, and MatchingEngine::configHash() keeps folding it
 *        in (it already does) so a snapshot cannot cross policies.
 */
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sequenced_shard.h"
#include "flox-venue/symbol_router.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <concepts>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

// Both flox and flox::venue declare a SymbolConfig; the venue's is the one
// an instrument is configured with.
using VenueConfig = flox::venue::SymbolConfig;

constexpr SymbolId SYM = 1;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

// Checked rather than assumed, so the tests below run and report instead of
// failing to compile: a missing member would otherwise take the controls in
// this file down with the red tests, and the point of the controls is that
// they still run.
template <class Cfg>
concept CarriesMatchPolicy = requires(Cfg c) {
  { c.matchPolicy } -> std::convertible_to<MatchPolicy>;
};

constexpr const char* kNoPolicyOnConfig =
    "SymbolConfig carries no matchPolicy: MatchPolicy::ProRata is reachable only by constructing "
    "a MatchingEngine by hand, so a pro-rata instrument cannot be given a shard, a journal, a "
    "checkpoint or a gateway";

VenueConfig cfg()
{
  VenueConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
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

// One level of 2 + 3 + 5 met by a buyer for 5. Pro-rata gives every maker half
// of what it showed (1.0 / 1.5 / 2.5); price-time gives the first two
// everything (2.0 / 3.0 / 0). The two rules are told apart by the numbers, not
// by a flag read back off the engine, so a policy that is stored and ignored
// fails these as loudly as one that is not stored at all.
const std::vector<InboundCommand>& oneLevelMetByFive()
{
  static const std::vector<InboundCommand> cmds{
      InboundCommand{limit(1, Side::SELL, 100.0, 2.0, 1)},
      InboundCommand{limit(2, Side::SELL, 100.0, 3.0, 1)},
      InboundCommand{limit(3, Side::SELL, 100.0, 5.0, 1)},
      InboundCommand{limit(9, Side::BUY, 100.0, 5.0, 2)}};
  return cmds;
}

struct Fills
{
  std::vector<OutboundEvent> ev;

  void take(const OutboundEvent& e) { ev.push_back(e); }

  Quantity forMaker(OrderId m) const
  {
    Quantity t{};
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<Trade>(&e); x != nullptr && x->makerId == m)
      {
        t += x->quantity;
      }
    }
    return t;
  }

  int trades() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      n += std::get_if<Trade>(&e) != nullptr ? 1 : 0;
    }
    return n;
  }
};

struct FillListener : IEngineEventListener
{
  Fills fills;
  void onEngineEvent(const EngineEventMsg& e) override { fills.take(e.event); }
};

void expectProRataSplit(const Fills& f)
{
  EXPECT_EQ(f.trades(), 3) << "pro-rata resolves every maker at the level, price-time stops early";
  EXPECT_EQ(f.forMaker(1), qty(1.0));
  EXPECT_EQ(f.forMaker(2), qty(1.5));
  EXPECT_EQ(f.forMaker(3), qty(2.5));
}

void cleanFiles(const std::string& base)
{
  std::remove(base.c_str());
  const auto g = SequencedShard<>::scanGenerations(base);
  std::error_code ec;
  for (const auto ts : g.snapshots)
  {
    std::filesystem::remove(SequencedShard<>::snapshotPath(base, ts), ec);
  }
  for (const auto ts : g.segments)
  {
    std::filesystem::remove(SequencedShard<>::segmentPath(base, ts), ec);
  }
}

template <class Cfg = VenueConfig>
void shardRunsProRata()
{
  if constexpr (!CarriesMatchPolicy<Cfg>)
  {
    ADD_FAILURE() << kNoPolicyOnConfig;
  }
  else
  {
    Cfg c = cfg();
    c.matchPolicy = MatchPolicy::ProRata;

    const std::string base = tmpPath("venue_match_policy_shard", ".bin");
    cleanFiles(base);

    FillListener sink;
    auto shard = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off);
    shard->setOwnThreads(false);
    ASSERT_TRUE(shard->subscribeOutbound(&sink));
    shard->start();
    for (const auto& cmd : oneLevelMetByFive())
    {
      shard->submit(cmd);
    }
    shard->flush();
    shard->stop();

    expectProRataSplit(sink.fills);
    EXPECT_GT(shard->journaled(), 0u) << "a pro-rata venue must journal like any other";

    shard.reset();
    cleanFiles(base);
  }
}

template <class Cfg = VenueConfig>
void routerRunsProRata()
{
  if constexpr (!CarriesMatchPolicy<Cfg>)
  {
    ADD_FAILURE() << kNoPolicyOnConfig;
  }
  else
  {
    Cfg c = cfg();
    c.matchPolicy = MatchPolicy::ProRata;

    Fills fills;
    SymbolRouter<MatchingBook> router(1);
    router.addSymbol(c, [&fills](const OutboundEvent& e)
                     { fills.take(e); });
    for (const auto& cmd : oneLevelMetByFive())
    {
      router.submit(cmd);
    }
    expectProRataSplit(fills);
  }
}

template <class Cfg = VenueConfig>
void policyIsPartOfTheShardConfigHash()
{
  if constexpr (!CarriesMatchPolicy<Cfg>)
  {
    ADD_FAILURE() << kNoPolicyOnConfig;
  }
  else
  {
    Cfg proRata = cfg();
    proRata.matchPolicy = MatchPolicy::ProRata;
    const Cfg priceTime = cfg();

    const std::string base = tmpPath("venue_match_policy_snapshot", ".bin");
    cleanFiles(base);

    {
      auto writer =
          std::make_unique<SequencedShard<>>(proRata, base, MatchingBook{}, Journal::Sync::Off);
      writer->setOwnThreads(false);
      writer->start();
      writer->submit(InboundCommand{limit(1, Side::SELL, 100.0, 2.0, 1)});
      writer->flush();
      ASSERT_TRUE(writer->checkpointNow());
      writer->flush();

      auto other =
          std::make_unique<SequencedShard<>>(priceTime, tmpPath("venue_match_policy_other", ".bin"),
                                             MatchingBook{}, Journal::Sync::Off);
      EXPECT_NE(writer->engine().configHash(), other->engine().configHash())
          << "two shards differing only in allocation rule share a config hash, so a snapshot "
             "taken under one rule loads into the other and every resting order is re-queued "
             "under an allocation it was never priced for";
      writer->stop();
      other->stop();
      cleanFiles(tmpPath("venue_match_policy_other", ".bin"));
    }

    // The snapshot itself: SnapshotBegin carries the writer's config hash and
    // an engine built under the other rule must turn it down.
    const auto gens = SequencedShard<>::scanGenerations(base);
    ASSERT_EQ(gens.snapshots.size(), 1u);
    const auto records = Journal::loadTimed(SequencedShard<>::snapshotPath(base, gens.snapshots[0]));
    ASSERT_FALSE(records.empty());

    MatchingEngine<MatchingBook> alien(priceTime, [](const OutboundEvent&) {});
    EXPECT_FALSE(alien.applySnapshotRecord(records.front().second, records.front().first))
        << "a pro-rata snapshot was accepted by a price-time engine";
    MatchingEngine<MatchingBook> same(proRata, [](const OutboundEvent&) {}, MatchingBook{}, MatchPolicy::ProRata);
    EXPECT_TRUE(same.applySnapshotRecord(records.front().second, records.front().first))
        << "the writer's own configuration no longer loads its own snapshot";

    cleanFiles(base);
  }
}

}  // namespace

// Control: the rule itself works and the numbers above are the right ones --
// what is missing is the way in. Green today.
TEST(VenueMatchPolicyReach, EngineBuiltByHandSplitsProRata)
{
  Fills fills;
  MatchingEngine<MatchingBook> eng(cfg(), [&fills](const OutboundEvent& e)
                                   { fills.take(e); }, MatchingBook{}, MatchPolicy::ProRata);
  for (const auto& cmd : oneLevelMetByFive())
  {
    eng.submit(cmd);
  }
  expectProRataSplit(fills);
}

// Control: the allocation rule is already folded into the constructor-config
// digest, so nothing below asks for a new hash -- only for a configuration
// that can name the rule. Green today.
TEST(VenueMatchPolicyReach, ConfigHashAlreadySeparatesTheTwoPolicies)
{
  MatchingEngine<MatchingBook> fifo(cfg(), [](const OutboundEvent&) {}, MatchingBook{}, MatchPolicy::PriceTimeFifo);
  MatchingEngine<MatchingBook> prorata(cfg(), [](const OutboundEvent&) {}, MatchingBook{}, MatchPolicy::ProRata);
  EXPECT_NE(fifo.configHash(), prorata.configHash());
}

TEST(VenueMatchPolicyReach, ShardRunsProRataWhenTheConfigNamesIt) { shardRunsProRata(); }

TEST(VenueMatchPolicyReach, RouterRunsProRataWhenTheConfigNamesIt) { routerRunsProRata(); }

TEST(VenueMatchPolicyReach, ShardSnapshotsDoNotCrossPolicies)
{
  policyIsPartOfTheShardConfigHash();
}

// Recovery through a checkpoint, on the shard's own rule.
//
// Before a snapshot generation is accepted it is replayed end to end into a
// scratch engine, and that probe is built from the same configuration the
// shard runs. It has to be: configHash folds the allocation rule, so a probe
// built under price-time refuses every generation a pro-rata shard ever
// published -- the shard falls back a generation, then another, and comes up
// on a full-history replay while its checkpoints sit on disk unused. Nothing
// about that is visible from the outside except the record count.
TEST(VenueMatchPolicyReach, ProRataShardRecoversThroughItsOwnSnapshot)
{
  VenueConfig c = cfg();
  c.matchPolicy = MatchPolicy::ProRata;

  const std::string base = tmpPath("venue_match_policy_probe", ".bin");
  cleanFiles(base);

  {
    auto writer = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off);
    writer->setOwnThreads(false);
    writer->start();
    writer->submit(InboundCommand{limit(1, Side::SELL, 100.0, 2.0, 1)});
    writer->submit(InboundCommand{limit(2, Side::SELL, 100.0, 3.0, 1)});
    writer->flush();
    ASSERT_TRUE(writer->checkpointNow());
    writer->flush();
    writer->stop();
  }

  FillListener sink;
  auto restarted = std::make_unique<SequencedShard<>>(c, base, MatchingBook{}, Journal::Sync::Off);
  restarted->setOwnThreads(false);
  ASSERT_TRUE(restarted->subscribeOutbound(&sink));
  restarted->start();
  EXPECT_GT(restarted->recoveredFromSnapshotRecords(), 0u)
      << "the shard turned down its own snapshot: the validating probe is built under an "
         "allocation rule the shard does not run, so no generation it writes can ever validate";

  // And the recovered shard is still pro-rata: the buyer meets the level the
  // snapshot restored and every maker on it is allocated a share.
  restarted->submit(InboundCommand{limit(9, Side::BUY, 100.0, 2.5, 2)});
  restarted->flush();
  EXPECT_EQ(sink.fills.trades(), 2) << "the recovered shard matched under price-time";
  EXPECT_EQ(sink.fills.forMaker(1), qty(1.0));
  EXPECT_EQ(sink.fills.forMaker(2), qty(1.5));

  restarted->stop();
  restarted.reset();
  cleanFiles(base);
}
