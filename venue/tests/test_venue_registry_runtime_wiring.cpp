/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * "The WAL is the configuration source of truth, not an external store."
 *
 * That is the contract written on InstrumentRegistry::apply, and apply() is
 * the only thing that can keep it: the registry is what the control plane
 * validates every request against, and a restart has to rebuild it from the
 * same journal the engines replay. Nothing in the shipped runtime calls it --
 * the shard replays its journal into the engine and into nothing else -- so a
 * restarted venue comes up with a registry that knows no instruments while its
 * engine knows all of its state. The second half of the same gap: apply() has
 * no branch for SetAccountRiskLimits, so the one configuration record added
 * most recently is answered "not a configuration command" while its three
 * siblings (SetRiskLimits, SetAdmissionProfile, SetStpGroup) are answered
 * true.
 *
 * These tests are written against the contract being kept -- apply() wired
 * into the shard's recovery. If the decision goes the other way, the honest
 * move is to rewrite the paragraph at control_plane.h:77-81 to say where
 * configuration actually lives and delete the recovery test below with it;
 * RegistryKnowsAccountRiskLimits stands either way, because a registry that
 * refuses one of its own configuration records is wrong under any contract.
 *
 * needs: void SequencedShard<Book>::setRegistry(InstrumentRegistry* reg) noexcept
 *        -- set before start(); every record the shard applies, replayed on
 *        start() and sequenced afterwards, is also offered to reg->apply().
 */
#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <concepts>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

using VenueConfig = flox::venue::SymbolConfig;

constexpr SymbolId SYM = 7;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

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

SetAccountRiskLimits accountCap(uint64_t account, double maxQty)
{
  SetAccountRiskLimits r;
  r.symbol = SYM;
  r.account = account;
  r.fields = AccountRiskLimitField::AccountRiskFatFinger;
  r.maxOrderQty = qty(maxQty);
  return r;
}

struct Listener : IEngineEventListener
{
  std::vector<OutboundEvent> ev;
  void onEngineEvent(const EngineEventMsg& e) override { ev.push_back(e.event); }
};

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

// Checked rather than assumed so the controls in this file still run when the
// wiring is not there yet.
template <class Shard>
concept TakesARegistry = requires(Shard& s, InstrumentRegistry& r) { s.setRegistry(&r); };

// One operator session against a live shard: list the instrument, put a band
// on it, switch its trigger reference and halt it. Every one of those is a
// sequenced record, so the journal at `base` afterwards IS the configuration.
void configureThroughTheControlPlane(const std::string& base)
{
  auto shard = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
  shard->setOwnThreads(false);
  shard->start();

  InstrumentRegistry reg;
  ControlApi api(reg, [&shard](const InboundCommand& c)
                 { shard->submit(c); });

  ASSERT_EQ(api.handle(R"({"method":"listInstrument","symbol":7,"tick":0.01,)"
                       R"("minPrice":50.0,"maxPrice":150.0})"),
            ControlApi::ok());
  ASSERT_EQ(api.handle(R"({"method":"setBand","symbol":7,"minPrice":90.0,"maxPrice":110.0})"),
            ControlApi::ok());
  ASSERT_EQ(api.handle(R"({"method":"setTriggerRef","symbol":7,"ref":"mark"})"),
            ControlApi::ok());
  ASSERT_EQ(api.handle(R"({"method":"halt","symbol":7,"halted":true})"), ControlApi::ok());

  shard->flush();
  shard->stop();
}

template <class Shard = SequencedShard<>>
void walRebuildsTheRegistry()
{
  if constexpr (!TakesARegistry<Shard>)
  {
    ADD_FAILURE()
        << "SequencedShard takes no InstrumentRegistry: nothing in the runtime calls "
           "InstrumentRegistry::apply, so a restarted venue rebuilds its engine from the WAL "
           "and its registry from nothing -- and the control plane validates every operator "
           "request against that empty registry";
  }
  else
  {
    const std::string base = tmpPath("venue_registry_wal", ".bin");
    cleanFiles(base);
    configureThroughTheControlPlane(base);

    InstrumentRegistry recovered;
    auto shard = std::make_unique<Shard>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
    shard->setOwnThreads(false);
    shard->setRegistry(&recovered);
    shard->start();

    EXPECT_GT(shard->recoveredCommands(), 0u) << "nothing was replayed: the journey is not set up";
    ASSERT_TRUE(recovered.has(SYM)) << "the instrument the WAL lists is unknown after a restart";
    const VenueConfig* c = recovered.get(SYM);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->minPrice, px(90.0));
    EXPECT_EQ(c->maxPrice, px(110.0));
    EXPECT_EQ(c->triggerRef, TriggerRef::Mark);
    EXPECT_TRUE(c->halted) << "the registry came up resumed while the engine came up halted";

    shard->stop();
    shard.reset();
    cleanFiles(base);
  }
}

}  // namespace

// Control: the registry already replays the records it knows, and refuses the
// ones that are not configuration. Green today -- it is the shape the record
// below is missing from.
TEST(VenueRegistryRuntimeWiring, ApplyKnowsTheConfigurationRecords)
{
  InstrumentRegistry reg;
  EXPECT_TRUE(reg.apply(InboundCommand{ListInstrument{SYM, {}, px(0.01), {}, px(50.0), px(150.0)}}));
  EXPECT_TRUE(reg.apply(InboundCommand{SetBands{SYM, {}, px(90.0), px(110.0)}}));
  EXPECT_TRUE(reg.apply(InboundCommand{SetTriggerRef{SYM, TriggerRef::Mark}}));
  EXPECT_TRUE(reg.apply(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}));
  EXPECT_TRUE(reg.apply(InboundCommand{SetRiskLimits{}}));
  EXPECT_TRUE(reg.apply(InboundCommand{SetAdmissionProfile{}}));
  EXPECT_FALSE(reg.apply(InboundCommand{limit(1, Side::BUY, 100.0, 1.0, 1)}))
      << "an order is not configuration";
}

// Control: the WAL already rebuilds the ENGINE across a restart -- the halt an
// operator set is still in force on the recovered shard. Green today, and what
// says the registry is the only half of the contract that is missing.
TEST(VenueRegistryRuntimeWiring, WalRebuildsTheEngineOnRestart)
{
  const std::string base = tmpPath("venue_registry_engine", ".bin");
  cleanFiles(base);
  configureThroughTheControlPlane(base);

  Listener listener;

  auto shard = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
  shard->setOwnThreads(false);
  ASSERT_TRUE(shard->subscribeOutbound(&listener));
  shard->start();
  EXPECT_GT(shard->recoveredCommands(), 0u);

  shard->submit(InboundCommand{limit(1, Side::BUY, 100.0, 1.0, 1)});
  shard->flush();
  bool halted = false;
  for (const auto& e : listener.ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e);
        r != nullptr && r->reason == RejectReason::Halted)
    {
      halted = true;
    }
  }
  EXPECT_TRUE(halted) << "the engine did not recover the halt from the WAL";

  shard->stop();
  shard.reset();
  cleanFiles(base);
}

// SetAccountRiskLimits is routed by symbol like SetAdmissionProfile
// (symbol_router.h), journaled, snapshotted and replayed -- and answered "not
// a configuration command" by the registry that replays the same stream.
TEST(VenueRegistryRuntimeWiring, ApplyKnowsAccountRiskLimits)
{
  InstrumentRegistry reg;
  ASSERT_TRUE(reg.apply(InboundCommand{ListInstrument{SYM, {}, px(0.01), {}, px(50.0), px(150.0)}}));
  EXPECT_TRUE(reg.apply(InboundCommand{accountCap(1, 1.0)}))
      << "the registry refuses a configuration record its own siblings are accepted on, so a "
         "replay of the WAL stops being a faithful replay at the first per-account limit";
}

TEST(VenueRegistryRuntimeWiring, WalRebuildsTheRegistryOnRestart) { walRebuildsTheRegistry(); }

// Recovery through a CHECKPOINT rather than through a full replay.
//
// A checkpoint rotates the journal: the records written before it live in a
// segment recovery never reads again, and the only surviving copy of the
// configuration they carried is the snapshot's own config section. So the
// shard has to offer that section to the registry too -- otherwise an
// instrument listed before the oldest surviving segment comes back unknown
// to the registry while the engine holds its whole state, and the control
// plane validates every operator request against the gap.
TEST(VenueRegistryRuntimeWiring, CheckpointRecoveryRebuildsTheRegistryFromTheSnapshot)
{
  const std::string base = tmpPath("venue_registry_snapshot", ".bin");
  cleanFiles(base);

  {
    auto shard =
        std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
    shard->setOwnThreads(false);
    shard->start();
    shard->submit(InboundCommand{ListInstrument{SYM, {}, px(0.01), {}, px(50.0), px(150.0)}});
    shard->submit(InboundCommand{SetBands{SYM, {}, px(90.0), px(110.0)}});
    shard->submit(InboundCommand{SetTriggerRef{SYM, TriggerRef::Mark}});
    shard->submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}});
    shard->flush();
    // The rotation: everything above is now behind the checkpoint boundary.
    ASSERT_TRUE(shard->checkpointNow());
    shard->flush();
    shard->stop();
  }

  InstrumentRegistry recovered;
  auto shard = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
  shard->setOwnThreads(false);
  shard->setRegistry(&recovered);
  shard->start();

  ASSERT_GT(shard->recoveredFromSnapshotRecords(), 0u)
      << "the snapshot was not the route back: this test says nothing about the config section";

  ASSERT_TRUE(recovered.has(SYM))
      << "recovery through a checkpoint left the registry empty: the configuration the snapshot "
         "carries is the only surviving copy of what the rotated-away segment said";
  const VenueConfig* c = recovered.get(SYM);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->minPrice, px(90.0));
  EXPECT_EQ(c->maxPrice, px(110.0));
  EXPECT_EQ(c->triggerRef, TriggerRef::Mark);
  EXPECT_TRUE(c->halted) << "the registry came up resumed while the engine came up halted";

  shard->stop();
  shard.reset();
  cleanFiles(base);
}

// A registry wired to a RUNNING shard.
//
// Replay at start() is half the contract. The other half is the live stream:
// a registry that is only rebuilt at start goes stale on the first command
// sequenced afterwards, and it is the live registry -- not the one from
// start-up -- that the control plane validates the NEXT operator request
// against. No restart, no checkpoint: the record and the registry move
// together or the registry is wrong.
TEST(VenueRegistryRuntimeWiring, ALiveCommandReachesTheRegistryWithoutARestart)
{
  const std::string base = tmpPath("venue_registry_live", ".bin");
  cleanFiles(base);

  InstrumentRegistry live;
  auto shard = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
  shard->setOwnThreads(false);
  shard->setRegistry(&live);
  shard->start();
  ASSERT_FALSE(live.has(SYM)) << "nothing was replayed: the registry starts empty";

  shard->submit(InboundCommand{ListInstrument{SYM, {}, px(0.01), {}, px(50.0), px(150.0)}});
  shard->flush();
  ASSERT_TRUE(live.has(SYM))
      << "a command the running shard sequenced never reached the registry: the registry the "
         "control plane validates against is the one from start-up";

  shard->submit(InboundCommand{SetBands{SYM, {}, px(90.0), px(110.0)}});
  shard->submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}});
  shard->flush();
  const VenueConfig* c = live.get(SYM);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->minPrice, px(90.0));
  EXPECT_EQ(c->maxPrice, px(110.0));
  EXPECT_TRUE(c->halted);

  shard->stop();
  shard.reset();
  cleanFiles(base);
}

namespace
{

// A book that refuses the Nth resting order. The shard is templated on the
// book, so the throw is injected through the type rather than through a hook
// added to production code for a test's benefit.
class BookThatRefuses : public MatchingBook
{
 public:
  BookThatRefuses() = default;
  explicit BookThatRefuses(int throwOnNth) : throwOnNth_(throwOnNth) {}

  [[nodiscard]] BookAddResult addResting(Side side, const RestingOrder& o)
  {
    if (++adds_ == throwOnNth_)
    {
      throw std::runtime_error("the book refused the order");
    }
    return MatchingBook::addResting(side, o);
  }

 private:
  int throwOnNth_ = 0;
  int adds_ = 0;
};

}  // namespace

// Write-ahead, stated as something observable.
//
// The journal is appended BEFORE the command is applied, and the order is the
// whole point: the engine is not transactional, so an apply that throws
// part-way leaves memory holding half a command. What makes that survivable
// is that the durable record is already complete -- a restart replays it and
// arrives at the state the live shard could not finish reaching. Applying
// first inverts that: the failure is now a command the engine (and anyone
// subscribed to it) saw and the journal never recorded, and the recovered
// venue silently disagrees with the one that was running.
//
// The observable difference is the record count after a refused apply: one
// under write-ahead, none if the apply goes first.
TEST(VenueRegistryRuntimeWiring, TheJournalHoldsTheRecordAnApplyThenThrewOn)
{
  const std::string base = tmpPath("venue_registry_writeahead", ".bin");
  cleanFiles(base);

  auto shard = std::make_unique<SequencedShard<BookThatRefuses>>(cfg(), base, BookThatRefuses{1},
                                                                 Journal::Sync::Off);
  shard->setOwnThreads(false);
  shard->start();

  shard->submit(InboundCommand{limit(1, Side::SELL, 100.0, 1.0, 1)});
  shard->flush();

  EXPECT_TRUE(shard->failed()) << "the shard kept serving after an apply threw";
  EXPECT_EQ(shard->journaled(), 1u)
      << "the record was not written ahead of the apply: a command the engine saw, and may have "
         "published events for, is missing from the history a restart would replay";

  shard->stop();
  shard.reset();
  cleanFiles(base);
}
