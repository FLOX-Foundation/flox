/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Admission profiles: what a counterparty is entitled to send.
//
// Two roles share one venue and have opposite rights. A counterparty routing
// flow it manages itself must never leave an order resting here -- an order the
// engine holds and the sender does not know it owns is a position nobody
// reconciles, and nothing about it looks wrong until it fills. A quoting
// counterparty needs exactly the opposite: rest, amend, cancel, repeat.
//
// The rights are refused on admission rather than trusted, because an
// entitlement that is documented and unenforced is the same thing as no
// entitlement at all.

#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <functional>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
const char* g_label = "";
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL [%s] line %d: %s\n", g_label, line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  return c;
}
LadderBook::Config lc() { return LadderBook::Config{0, px(0.01).raw(), 200000, 1 << 16}; }

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

constexpr uint32_t typeBit(OrderType t) { return 1u << static_cast<uint32_t>(t); }
constexpr uint32_t tifBit(TimeInForce t) { return 1u << static_cast<uint32_t>(t); }

// The profile a counterparty gets when it manages its own order book and only
// sends what is meant to execute now.
AdmissionProfile takerOnly()
{
  AdmissionProfile p;
  p.allowedTypes = typeBit(OrderType::LIMIT) | typeBit(OrderType::MARKET);
  p.allowedTif = tifBit(TimeInForce::IOC) | tifBit(TimeInForce::FOK);
  p.deny = AdmissionDeny::DenyResting | AdmissionDeny::DenyAmend | AdmissionDeny::DenyCancel |
           AdmissionDeny::DenyQuote;
  return p;
}

struct Cap
{
  std::vector<OutboundEvent> ev;
  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }
  int rejects(RejectReason r) const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (auto* x = std::get_if<OrderRejected>(&e); x && x->reason == r)
      {
        ++n;
      }
    }
    return n;
  }
  // A refused cancel or replace is its own event, so the assertion names it
  // and says which request it answered. Counting it as a plain reject would
  // pass even if the discriminator were wrong.
  int cancelRejects(RejectReason r, bool wasReplace) const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (auto* x = std::get_if<CancelRejected>(&e);
          x && x->reason == r && x->wasReplace == wasReplace)
      {
        ++n;
      }
    }
    return n;
  }
  int trades() const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (std::get_if<Trade>(&e))
      {
        ++n;
      }
    }
    return n;
  }
};

template <class Book>
void run(const std::function<Book()>& mk, const char* label)
{
  g_label = label;
  std::printf("== admission book: %s ==\n", label);

  {  // The order that must never happen: a GTC limit from a taker-only
     // counterparty. Refused on admission, not killed after partially filling,
     // so no residual can ever reach the book.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setAdmissionProfile(7, takerOnly());
    eng.submit(InboundCommand{limit(1, Side::BUY, 100, 5, 7)});  // GTC by default
    CHECK(cap.rejects(RejectReason::RestingNotPermitted) == 1);
    CHECK(eng.book().empty());
    CHECK(eng.admissionRejects() == 1);
  }
  {  // The same counterparty sending what it is entitled to: IOC executes
     // against resting liquidity and leaves nothing behind.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setAdmissionProfile(7, takerOnly());
    eng.submit(InboundCommand{limit(1, Side::SELL, 100, 2, 99)});  // a maker, unprofiled
    NewOrder take = limit(2, Side::BUY, 100, 5, 7);
    take.tif = TimeInForce::IOC;
    eng.submit(InboundCommand{take});
    CHECK(cap.trades() == 1);
    CHECK(eng.book().empty());  // the unfilled 3 did not rest
    CHECK(eng.admissionRejects() == 0);
  }
  {  // A conditional order is not in the profile's type set, so it is refused
     // rather than parked. A stop living in two places at once is the failure
     // this exists to prevent.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setAdmissionProfile(7, takerOnly());
    NewOrder stop = limit(1, Side::BUY, 100, 5, 7);
    stop.type = OrderType::STOP_MARKET;
    stop.triggerPrice = px(101);
    stop.tif = TimeInForce::IOC;
    eng.submit(InboundCommand{stop});
    CHECK(cap.rejects(RejectReason::OrderTypeNotPermitted) == 1);
    CHECK(eng.admissionRejects() == 1);
  }
  {  // post-only is a request to rest and nothing else.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setAdmissionProfile(7, takerOnly());
    NewOrder po = limit(1, Side::BUY, 99, 5, 7);
    po.tif = TimeInForce::IOC;
    po.postOnly = true;
    eng.submit(InboundCommand{po});
    CHECK(cap.rejects(RejectReason::RestingNotPermitted) == 1);
    CHECK(eng.book().empty());
  }
  {  // Amend, cancel and quote are refused for a counterparty that owns none
     // of those, and each says which right was missing.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setAdmissionProfile(7, takerOnly());
    eng.submit(InboundCommand{ModifyOrder{1, SYM, px(100), qty(1), 7}});
    eng.submit(InboundCommand{CancelOrder{1, SYM, 7}});
    eng.submit(InboundCommand{Quote{10, 11, SYM, px(99), qty(1), px(101), qty(1), 7}});
    CHECK(cap.cancelRejects(RejectReason::AmendNotPermitted, /*wasReplace*/ true) == 1);
    CHECK(cap.cancelRejects(RejectReason::CancelNotPermitted, /*wasReplace*/ false) == 1);
    CHECK(cap.rejects(RejectReason::QuoteNotPermitted) == 1);
    CHECK(eng.admissionRejects() == 3);
  }
  {  // A quoting counterparty is the opposite role and is untouched: it rests,
     // amends and cancels as before.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    AdmissionProfile mm;
    mm.allowedTypes = typeBit(OrderType::LIMIT);
    mm.allowedTif = tifBit(TimeInForce::GTC) | tifBit(TimeInForce::POST_ONLY);
    eng.setAdmissionProfile(8, mm);
    eng.submit(InboundCommand{limit(1, Side::BUY, 99, 5, 8)});
    CHECK(eng.book().find(1) != nullptr);
    eng.submit(InboundCommand{ModifyOrder{1, SYM, px(98), qty(4), 8}});
    eng.submit(InboundCommand{CancelOrder{1, SYM, 8}});
    CHECK(eng.book().empty());
    CHECK(eng.admissionRejects() == 0);
  }
  {  // No profile means no restriction: an engine that was never given one
     // behaves exactly as it always did.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(InboundCommand{limit(1, Side::BUY, 99, 5, 7)});
    CHECK(eng.book().find(1) != nullptr);
    CHECK(eng.admissionRejects() == 0);
  }
  {  // A refused order consumes nothing: the clientOrderId it carried is still
     // free, so the counterparty can correct the message and resend it.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setAdmissionProfile(7, takerOnly());
    NewOrder bad = limit(1, Side::BUY, 100, 5, 7);
    bad.clientOrderId = 555;
    eng.submit(InboundCommand{bad});
    CHECK(cap.rejects(RejectReason::RestingNotPermitted) == 1);
    NewOrder good = limit(2, Side::BUY, 100, 5, 7);
    good.clientOrderId = 555;  // same id, corrected message
    good.tif = TimeInForce::IOC;
    eng.submit(InboundCommand{good});
    CHECK(cap.rejects(RejectReason::DuplicateClientOrderId) == 0);
  }
  {  // The profile is engine state and rides the state hash, so two replicas
     // holding different profiles cannot agree.
     //
     // Both engines are fed the same NUMBER of commands at the same
     // timestamps throughout: the sequencer counter is itself part of the
     // hash, so a test that let the command counts drift would pass on that
     // difference alone and prove nothing about the profile.
    Cap ca, cb;
    MatchingEngine<Book> a(cfg(), ca.sink(), mk());
    MatchingEngine<Book> b(cfg(), cb.sink(), mk());
    CHECK(a.stateHash() == b.stateHash());  // baseline: identical before anything

    a.submit(InboundCommand{SetAdmissionProfile{SYM, 7, takerOnly()}}, 1);
    b.submit(InboundCommand{SetAdmissionProfile{SYM, 8, takerOnly()}}, 1);
    CHECK(a.stateHash() != b.stateHash());  // same command count, different state

    // Each learns the other's profile. The table is keyed and sorted, so the
    // order they arrived in must not matter either.
    a.submit(InboundCommand{SetAdmissionProfile{SYM, 8, takerOnly()}}, 2);
    b.submit(InboundCommand{SetAdmissionProfile{SYM, 7, takerOnly()}}, 2);
    CHECK(a.stateHash() == b.stateHash());
  }
  {  // Risk limits arrive as a command, so they survive what a direct setter
     // does not: a replica that replayed the journal agrees, and a checkpoint
     // carries them. Only the named limits move.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setMaxOpenOrders(5);
    eng.setPositionLimit(qty(100));

    SetRiskLimits r;
    r.symbol = SYM;
    r.fields = RiskLimitField::RiskMaxOpenOrders;
    r.maxOpenOrders = 1;
    eng.submit(InboundCommand{r});

    // Named limit applied; the one not named is untouched.
    eng.submit(InboundCommand{limit(1, Side::BUY, 99, 1, 7)});
    eng.submit(InboundCommand{limit(2, Side::BUY, 98, 1, 7)});
    CHECK(cap.rejects(RejectReason::TooManyOpenOrders) == 1);
    CHECK(eng.riskLimits().maxPositionQty == qty(100));
  }
  {  // A limit set through the command is part of the state two replicas must
     // agree on, and it reproduces through a snapshot.
    Cap ca, cb;
    MatchingEngine<Book> a(cfg(), ca.sink(), mk());
    MatchingEngine<Book> b(cfg(), cb.sink(), mk());
    SetRiskLimits r;
    r.symbol = SYM;
    r.fields = RiskLimitField::RiskFatFinger;
    r.maxOrderQty = qty(3);
    a.submit(InboundCommand{r}, 1);
    b.submit(InboundCommand{r}, 1);
    CHECK(a.riskLimits().maxOrderQty == qty(3));
    CHECK(b.riskLimits().maxOrderQty == qty(3));
    a.submit(InboundCommand{limit(1, Side::BUY, 99, 10, 7)});
    CHECK(ca.rejects(RejectReason::OrderTooLarge) == 1);
  }
  {  // Delisting is not a halt and not a closed session: it pulls the resting
     // book, because there is no open to wait for. New orders earn a reason
     // that says so rather than one that promises a return.
    Cap cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(InboundCommand{limit(1, Side::BUY, 99, 5, 7)});
    CHECK(eng.book().find(1) != nullptr);

    eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Delist}});
    CHECK(eng.delisted());
    CHECK(eng.tradingStatus() == TradingStatus::Delisted);
    CHECK(eng.book().empty());  // the book went with it

    eng.submit(InboundCommand{limit(2, Side::BUY, 99, 5, 7)});
    CHECK(cap.rejects(RejectReason::InstrumentDelisted) == 1);

    // Delisting outranks everything under it: reopening the session does not
    // make a delisted instrument tradeable.
    eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::OpenSession}});
    eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Resume}});
    CHECK(eng.tradingStatus() == TradingStatus::Delisted);
    eng.submit(InboundCommand{limit(3, Side::BUY, 99, 5, 7)});
    CHECK(cap.rejects(RejectReason::InstrumentDelisted) == 2);

    // Reversible: an irreversible operator action is one mistake away from
    // needing a restart to undo.
    eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Relist}});
    CHECK(eng.tradingStatus() == TradingStatus::Trading);
    eng.submit(InboundCommand{limit(4, Side::BUY, 99, 5, 7)});
    CHECK(eng.book().find(4) != nullptr);
  }
}

}  // namespace

TEST(Admission, EngineSuite)
{
  run<MatchingBook>([]
                    { return MatchingBook{}; }, "map-reference");
  run<LadderBook>([]
                  { return LadderBook{lc()}; }, "ladder-o1");
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
