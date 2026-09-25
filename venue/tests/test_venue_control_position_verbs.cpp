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
#include <limits>
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

// A correction carries no fill behind it, so the record IS the explanation.
// A reason the venue does not recognize is an operator typo, and mapping it
// onto "manual" writes a plausible word onto a correction nobody
// characterized -- a later reader cannot then tell a routine reconciliation
// from a mistake, and the wrong word is durable.
TEST(VenueControlPositionVerbs, AdjustPositionRefusesAnUnknownReason)
{
  Desk d("venue_cpv_badreason");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"adjustPosition","symbol":7,"account":1,"qtyDelta":-2.0,)"
      R"("reason":"reconcilliation"})");
  EXPECT_NE(reply.find("bad_reason"), std::string::npos)
      << "an unrecognized reason was accepted: a typo becomes 'operator judgement' in a record "
         "nothing can tell from a real manual correction";
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before) << "a refused correction was journaled";
  EXPECT_EQ(d.shard->engine().positionQty(OWNER), qty(5).raw());

  // An empty reason is the same refusal, not a default.
  const std::string blank =
      d.ask(R"({"method":"adjustPosition","symbol":7,"account":1,"qtyDelta":-2.0})");
  EXPECT_NE(blank.find("bad_reason"), std::string::npos);
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before);
}

// qty is a SIZE. Which way the close goes is decided by the position, so a
// signed request would let an operator name a direction that contradicts it,
// and the record has no way to carry the disagreement to whoever reads it.
TEST(VenueControlPositionVerbs, ForceClosePositionRefusesASignedQty)
{
  Desk d("venue_cpv_signedqty");
  ASSERT_EQ(d.shard->engine().positionQty(OWNER), qty(5).raw());
  const uint64_t before = d.journaled();

  const std::string reply =
      d.ask(R"({"method":"forceClosePosition","symbol":7,"account":1,"qty":-2.0})");
  EXPECT_NE(reply.find("bad_field"), std::string::npos)
      << "a negative size was accepted on a field whose sign the record cannot express";
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before) << "a refused close was journaled";
  EXPECT_EQ(d.shard->engine().positionQty(OWNER), qty(5).raw());

  // The same size without the sign is the request that was meant, and it is
  // taken -- so the refusal above is about the sign and nothing else.
  const std::string good =
      d.ask(R"({"method":"forceClosePosition","symbol":7,"account":1,"qty":2.0})");
  EXPECT_EQ(good, ControlApi::ok());
  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* fc = std::get_if<ForceClosePosition>(&d.forwarded.front());
  ASSERT_NE(fc, nullptr);
  EXPECT_EQ(fc->qtyRaw, qty(2.0).raw());
}

// A request that names none of the three limit groups asks for nothing. Its
// symbol-wide sibling refuses the same shape, and it has to: forwarded, the
// record is a no-op that the journal keeps forever and a replay re-applies,
// and the operator gets an "ok" for a limit that was never set.
TEST(VenueControlPositionVerbs, SetAccountRiskLimitsRefusesARequestNamingNoLimits)
{
  Desk d("venue_cpv_emptymask");
  const uint64_t before = d.journaled();

  const std::string reply =
      d.ask(R"({"method":"setAccountRiskLimits","symbol":7,"account":1})");
  EXPECT_NE(reply.find("no_limits_named"), std::string::npos)
      << "a request naming no limit was accepted and journaled as a no-op";
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before) << "an empty limit record reached the journal";
  EXPECT_EQ(d.shard->engine().accountRiskLimits(OWNER), nullptr);
}

// The top of maxOpenOrders' own range. The field is a uint32_t, so
// UINT32_MAX is a value it can hold and a value an owner can legitimately
// name; one past it is not, and is refused rather than truncated into a cap
// far tighter than the one asked for.
TEST(VenueControlPositionVerbs, SetAccountRiskLimitsTakesTheTopOfTheMaxOpenOrdersRange)
{
  Desk d("venue_cpv_maxopenorders");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"setAccountRiskLimits","symbol":7,"account":1,"maxOpenOrders":4294967295})");
  EXPECT_EQ(reply, ControlApi::ok())
      << "the largest value the field can hold was refused as out of range";
  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* r = std::get_if<SetAccountRiskLimits>(&d.forwarded.front());
  ASSERT_NE(r, nullptr);
  EXPECT_NE(r->fields & AccountRiskLimitField::AccountRiskMaxOpenOrders, 0);
  EXPECT_EQ(r->maxOpenOrders, (std::numeric_limits<uint32_t>::max)());
  EXPECT_EQ(d.journaled(), before + 1);

  // One past the top does not fit, and is refused rather than wrapped.
  d.forwarded.clear();
  const uint64_t afterFirst = d.journaled();
  const std::string tooBig = d.ask(
      R"({"method":"setAccountRiskLimits","symbol":7,"account":1,"maxOpenOrders":4294967296})");
  EXPECT_NE(tooBig.find("bad_field"), std::string::npos);
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), afterFirst);
}

// The registry is what the control plane validates against, and the check is
// per verb: a symbol the venue never listed has no shard to route to, so the
// record would be sequenced into whichever engine the sink happens to be and
// then refused there -- after the journal already holds it. Refused at the
// edge instead, where the refusal costs nothing and names what is wrong.
TEST(VenueControlPositionVerbs, SetAccountRiskLimitsRefusesAnUnlistedSymbol)
{
  Desk d("venue_cpv_acctrisk_unknown");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"setAccountRiskLimits","symbol":8,"account":1,"maxOrderQty":1.0,)"
      R"("maxOrderNotional":0})");
  EXPECT_NE(reply.find("unknown_symbol"), std::string::npos)
      << "a limit was accepted for an instrument the registry does not know";
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before) << "a limit on an unlisted symbol reached the journal";
}

// The same check on the same footing for the close: an owner cannot close a
// position on an instrument this venue does not carry.
TEST(VenueControlPositionVerbs, ForceClosePositionRefusesAnUnlistedSymbol)
{
  Desk d("venue_cpv_forceclose_unknown");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(R"({"method":"forceClosePosition","symbol":8,"account":1})");
  EXPECT_NE(reply.find("unknown_symbol"), std::string::npos)
      << "a close was accepted for an instrument the registry does not know";
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before) << "a close on an unlisted symbol reached the journal";
}

// maxOrderQty and maxOrderNotional are ONE limit under one mask bit: a size
// cap with no notional cap is a fat-finger guard an operator can walk
// straight past by pricing the order up, and the record cannot express "one
// of the pair is set" -- the bit covers both fields. So half a pair is
// refused rather than silently completed with a zero the engine would read
// as "no notional limit at all".
TEST(VenueControlPositionVerbs, SetAccountRiskLimitsRefusesHalfOfTheFatFingerPair)
{
  Desk d("venue_cpv_halfpair");
  const uint64_t before = d.journaled();

  const std::string qtyOnly =
      d.ask(R"({"method":"setAccountRiskLimits","symbol":7,"account":1,"maxOrderQty":1.0})");
  EXPECT_NE(qtyOnly.find("bad_field"), std::string::npos)
      << "a fat-finger cap was set from half of its pair: the other half is not unset, it is "
         "written as zero and read as 'no limit'";
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before);

  const std::string notionalOnly = d.ask(
      R"({"method":"setAccountRiskLimits","symbol":7,"account":1,"maxOrderNotional":100.0})");
  EXPECT_NE(notionalOnly.find("bad_field"), std::string::npos);
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before);

  // Named as a pair it is taken, and the one bit that covers both is set --
  // so the refusals above are about the missing half and nothing else.
  const std::string both = d.ask(
      R"({"method":"setAccountRiskLimits","symbol":7,"account":1,"maxOrderQty":1.0,)"
      R"("maxOrderNotional":250.0})");
  EXPECT_EQ(both, ControlApi::ok());
  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* r = std::get_if<SetAccountRiskLimits>(&d.forwarded.front());
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->fields, static_cast<uint16_t>(AccountRiskLimitField::AccountRiskFatFinger))
      << "the pair set a mask bit that is not the fat-finger one";
  EXPECT_EQ(r->maxOrderQty, qty(1.0));
  EXPECT_EQ(r->maxOrderNotional, Volume::fromDouble(250.0));
  EXPECT_EQ(d.journaled(), before + 1);
}

// Each limit group owns its own bit. A request naming only the position cap
// must journal exactly that bit: masked as fat-finger instead, the engine
// applies a zero order-size cap the operator never asked for and leaves the
// position uncapped -- the two failures compound, and both survive a replay.
TEST(VenueControlPositionVerbs, SetAccountRiskLimitsMasksTheMaxPositionGroupOnItsOwnBit)
{
  Desk d("venue_cpv_maxposition");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"setAccountRiskLimits","symbol":7,"account":1,"maxPositionQty":10.0})");
  EXPECT_EQ(reply, ControlApi::ok());
  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* r = std::get_if<SetAccountRiskLimits>(&d.forwarded.front());
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->fields, static_cast<uint16_t>(AccountRiskLimitField::AccountRiskMaxPosition))
      << "a position cap was masked onto another group's bit";
  EXPECT_EQ(r->maxPositionQty, qty(10.0));
  EXPECT_EQ(r->maxOrderQty, qty(0.0)) << "a field nobody named carries no value";
  EXPECT_EQ(d.journaled(), before + 1);

  const auto* live = d.shard->engine().accountRiskLimits(OWNER);
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(live->maxPositionQty, qty(10.0));
}

// An entry price is a price. A negative one is not a correction the venue can
// carry: the record has no way to say "this entry is nonsense", so every
// later PnL on the account is computed from it and the journal keeps it.
TEST(VenueControlPositionVerbs, AdjustPositionRefusesANegativeEntry)
{
  Desk d("venue_cpv_negentry");
  const uint64_t before = d.journaled();

  const std::string reply = d.ask(
      R"({"method":"adjustPosition","symbol":7,"account":1,"qtyDelta":-2.0,"entry":-100.0,)"
      R"("reason":"reconciliation"})");
  EXPECT_NE(reply.find("bad_field"), std::string::npos)
      << "a negative entry price was journaled: every later PnL on the account is computed "
         "from it";
  EXPECT_TRUE(d.forwarded.empty());
  EXPECT_EQ(d.journaled(), before);
  EXPECT_EQ(d.shard->engine().positionQty(OWNER), qty(5).raw());

  // The same correction with a real entry is taken, so the refusal is about
  // the sign of the price and nothing else.
  const std::string good = d.ask(
      R"({"method":"adjustPosition","symbol":7,"account":1,"qtyDelta":-2.0,"entry":100.0,)"
      R"("reason":"reconciliation"})");
  EXPECT_EQ(good, ControlApi::ok());
  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* a = std::get_if<AdjustPosition>(&d.forwarded.front());
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->entryRaw, px(100.0).raw());
}

// The note is memcpy'd into a fixed-length field in the journal, so the copy
// has to leave room for the terminator. Filling the last byte makes the
// record's note run into whatever follows it: every reader of the journal --
// an operator's extract, a dispute bundle -- reads past the field.
TEST(VenueControlPositionVerbs, AdjustPositionTruncatesALongNoteAndKeepsItTerminated)
{
  Desk d("venue_cpv_longnote");

  // Longer than the field, and every character distinct enough that a
  // truncation at the wrong offset is visible in the failure message.
  const std::string long_note = "0123456789abcdefghijklmnopqrstuvwxyzABCD";
  ASSERT_GT(long_note.size(), kAdjustNoteLen);

  const std::string reply =
      d.ask(R"({"method":"adjustPosition","symbol":7,"account":1,"qtyDelta":-1.0,)"
            R"("reason":"manual","note":")" +
            long_note + R"("})");
  EXPECT_EQ(reply, ControlApi::ok());
  ASSERT_EQ(d.forwarded.size(), 1u);
  const auto* a = std::get_if<AdjustPosition>(&d.forwarded.front());
  ASSERT_NE(a, nullptr);

  ASSERT_EQ(a->note[kAdjustNoteLen - 1], '\0')
      << "the copy filled the last byte of the note field: the record has no terminator and "
         "every reader of it runs past the field";
  EXPECT_EQ(std::string(a->note), long_note.substr(0, kAdjustNoteLen - 1))
      << "the note was truncated somewhere other than the last writable character";
}
