/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Three journaled commands with no operator surface.
 *
 * SetAccountRiskLimits, AdjustPosition and ForceClosePosition are sequenced
 * commands: each is journaled before it is applied, written into the
 * snapshot's config section or reproduced by replay, and documented in
 * docs/venue/matching.md and docs/venue/risk.md as the way an owner tightens
 * an account, corrects a position by hand, or closes one on someone else's
 * decision. The control plane has no verb for any of the three, so the only
 * way to send one is to reach into the process and call submit() -- which is
 * exactly what those docs tell an operator not to do ("It is a command, not a
 * setter, for the same reason as everything else here").
 *
 * The tests below drive the same path an operator has: a JSON request into
 * ControlApi::handle, whose sink is the shard. What they check of each verb is
 * the whole contract -- the request is accepted, the record it forwards is the
 * right one, the shard journals it, and the engine state moves.
 *
 * needs: three built-in verbs in ControlApi::handle, listed in kBuiltinMethods
 *        so registerMethod keeps refusing them and test_venue_control_methods
 *        keeps walking them:
 *
 *   {"method":"setAccountRiskLimits","symbol":<id>,"account":<n>,
 *    "maxOrderQty":<dec>,"maxOrderNotional":<dec>,   // named as a pair, like setRiskLimits
 *    "maxOpenOrders":<n>,"maxPositionQty":<dec>}     // each optional, masked
 *   {"method":"adjustPosition","symbol":<id>,"account":<n>,"qtyDelta":<signed dec>,
 *    "entry":<dec>,           // optional, 0 / absent = keep the average entry
 *    "reason":"reconciliation"|"counterpartyReport"|"settlementCorrection"|"migration"|"manual",
 *    "note":"<text>"}         // optional, truncated to kAdjustNoteLen - 1
 *   {"method":"forceClosePosition","symbol":<id>,"account":<n>,"qty":<dec>}
 *                             // qty optional, 0 / absent = the whole position
 */
#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

using VenueConfig = flox::venue::SymbolConfig;

constexpr SymbolId SYM = 7;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 999;
constexpr uint64_t OWNER = 1;
constexpr uint64_t OTHER = 2;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

VenueConfig cfg()
{
  VenueConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
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

// An operator on one side and a running shard on the other, wired the way
// docs/venue/runtime.md wires them: the control plane's sink IS the sequenced
// path, so a verb that forwards a record has journaled it by the time the
// reply goes out.
//
// The instrument is a linear perp carrying one open position -- account 1 long
// 5 at 100 against account 2 -- and a mark, because that is the state the
// three commands act on.
struct Desk
{
  std::string base;
  Ledger led;
  std::unique_ptr<SequencedShard<>> shard;
  InstrumentRegistry reg;
  std::vector<InboundCommand> forwarded;
  std::unique_ptr<ControlApi> api;

  explicit Desk(const char* stem) : base(tmpPath(stem, ".bin"))
  {
    cleanFiles(base);
    shard = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
    shard->setOwnThreads(false);
    led.deposit(OWNER, QUOTE, static_cast<Amount>(10000) * 100000000);
    led.deposit(OTHER, QUOTE, static_cast<Amount>(10000) * 100000000);
    shard->engine().setLedger(&led, VENUE_ACCT);
    shard->start();

    shard->submit(InboundCommand{limit(1, Side::SELL, 100.0, 5.0, OTHER)});
    shard->submit(InboundCommand{limit(2, Side::BUY, 100.0, 5.0, OWNER)});
    shard->submit(InboundCommand{SetMark{SYM, {}, px(100.0)}});
    shard->flush();

    reg.listInstrument(cfg());
    api = std::make_unique<ControlApi>(reg, [this](const InboundCommand& c)
                                       {
                                         forwarded.push_back(c);
                                         shard->submit(c); });
  }

  ~Desk()
  {
    shard->stop();
    shard.reset();
    cleanFiles(base);
  }

  uint64_t journaled() const { return shard->journaled(); }

  // The operator's round trip: one request in, the shard stepped, the reply
  // out. Returned rather than asserted on here so each test says what it
  // expected of the answer.
  std::string ask(const std::string& request)
  {
    const std::string reply = api->handle(request);
    shard->flush();
    return reply;
  }
};

}  // namespace

// Control: the harness itself. An existing verb travels the whole path --
// accepted, forwarded as the record it names, journaled by the shard, applied
// by the engine. Green today, and what says a red test below is about the
// missing verb rather than about the wiring.
TEST(VenueControlPositionVerbs, SetRiskLimitsTravelsTheWholePath)
{
  Desk d("venue_cpv_control");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"setRiskLimits","symbol":7,"maxOrderQty":1.0,"maxOrderNotional":0})");
  EXPECT_EQ(reply, ControlApi::ok());
  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* r = std::get_if<SetRiskLimits>(&d.forwarded.front());
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->symbol, SYM);
  EXPECT_EQ(d.journaled(), before + 1);

  // Applied: the symbol's new fat-finger cap refuses an order it used to take.
  d.shard->submit(InboundCommand{limit(3, Side::BUY, 100.0, 5.0, OWNER)});
  d.shard->flush();
  EXPECT_EQ(d.shard->engine().snapshotAccount(OWNER).openOrders.size(), 0u);
}

// Control: an unknown verb is refused by name, which is what each of the three
// below gets today. Green.
TEST(VenueControlPositionVerbs, UnknownVerbIsRefusedByName)
{
  Desk d("venue_cpv_unknown");
  const std::string reply = d.ask(R"({"method":"notAVerb","symbol":7})");
  EXPECT_NE(reply.find("unknown_method"), std::string::npos);
  EXPECT_NE(reply.find("notAVerb"), std::string::npos);
  EXPECT_TRUE(d.forwarded.empty());
}

// An owner of risk above the venue tightens ONE account, through the journal,
// from outside the process.
TEST(VenueControlPositionVerbs, SetAccountRiskLimitsHasAVerb)
{
  Desk d("venue_cpv_acctrisk");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"setAccountRiskLimits","symbol":7,"account":1,"maxOrderQty":1.0,"maxOrderNotional":0})");
  EXPECT_EQ(reply, ControlApi::ok())
      << "no operator surface for SetAccountRiskLimits: the record is journaled, snapshotted and "
         "replayed, and the only way to send one is to call submit() in-process";

  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* r = std::get_if<SetAccountRiskLimits>(&d.forwarded.front());
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->symbol, SYM);
  EXPECT_EQ(r->account, OWNER);
  EXPECT_NE(r->fields & AccountRiskLimitField::AccountRiskFatFinger, 0);
  EXPECT_EQ(r->maxOrderQty, qty(1.0));
  EXPECT_EQ(d.journaled(), before + 1) << "the limit was not journaled before it was applied";

  const auto* live = d.shard->engine().accountRiskLimits(OWNER);
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(live->maxOrderQty, qty(1.0));

  // And the tightened account is the only one bound by it.
  EXPECT_EQ(d.shard->engine().accountRiskLimits(OTHER), nullptr);
}

// The operator books a difference an external record reports. Not a trade: the
// position moves and nothing else does.
TEST(VenueControlPositionVerbs, AdjustPositionHasAVerb)
{
  Desk d("venue_cpv_adjust");
  ASSERT_EQ(d.shard->engine().positionQty(OWNER), qty(5).raw());
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"adjustPosition","symbol":7,"account":1,"qtyDelta":-2.0,)"
      R"("reason":"counterpartyReport","note":"LP fill 88213"})");
  EXPECT_EQ(reply, ControlApi::ok())
      << "no operator surface for AdjustPosition: docs/venue/matching.md says a correction must "
         "be a command rather than a setter, and there is no way to send the command";

  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* a = std::get_if<AdjustPosition>(&d.forwarded.front());
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->symbol, SYM);
  EXPECT_EQ(a->accountId, OWNER);
  EXPECT_EQ(a->qtyDeltaRaw, -qty(2.0).raw());
  EXPECT_EQ(a->entryRaw, 0) << "an entry nobody named must stay unset, not become a zero entry";
  EXPECT_EQ(a->reason, AdjustReason::CounterpartyReport);
  EXPECT_STREQ(a->note, "LP fill 88213");
  EXPECT_EQ(d.journaled(), before + 1);

  EXPECT_EQ(d.shard->engine().positionQty(OWNER), qty(3).raw());
}

// The owner of risk closes a position the engine is not allowed to judge.
TEST(VenueControlPositionVerbs, ForceClosePositionHasAVerb)
{
  Desk d("venue_cpv_forceclose");
  ASSERT_EQ(d.shard->engine().positionQty(OWNER), qty(5).raw());
  const uint64_t before = d.journaled();

  const std::string reply =
      d.ask(R"({"method":"forceClosePosition","symbol":7,"account":1})");
  EXPECT_EQ(reply, ControlApi::ok())
      << "no operator surface for ForceClosePosition: docs/venue/risk.md makes it the owner's "
         "decision and the owner cannot reach it";

  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* fc = std::get_if<ForceClosePosition>(&d.forwarded.front());
  ASSERT_NE(fc, nullptr);
  EXPECT_EQ(fc->symbol, SYM);
  EXPECT_EQ(fc->accountId, OWNER);
  EXPECT_EQ(fc->qtyRaw, 0) << "an unnamed size closes the whole position";
  EXPECT_EQ(d.journaled(), before + 1);

  EXPECT_EQ(d.shard->engine().positionQty(OWNER), 0);
}

// A verb answered by handle() but missing from kBuiltinMethods is a name a
// deployment can register over, and the registration wins for some requests
// and loses for others depending on where the name is read. The list is the
// single answer to "is this name taken".
TEST(VenueControlPositionVerbs, TheThreeVerbsAreListedAsBuiltins)
{
  for (const char* name : {"setAccountRiskLimits", "adjustPosition", "forceClosePosition"})
  {
    EXPECT_TRUE(ControlApi::isBuiltinMethod(name))
        << name << " is answered by nothing and reserved by nothing";
  }

  InstrumentRegistry reg;
  ControlApi api(reg);
  for (const char* name : {"setAccountRiskLimits", "adjustPosition", "forceClosePosition"})
  {
    EXPECT_FALSE(api.registerMethod(name, [](const ControlRequest&)
                                    { return ControlApi::ok(); }))
        << name << " can be registered by a deployment, shadowing the built-in that owns it";
  }
}
