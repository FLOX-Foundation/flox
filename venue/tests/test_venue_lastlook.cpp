/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Last-look lifecycle (T016) and clientOrderId dedup (T020).
 *
 * Last look: a reject/timeout must RESTORE liquidity -- the maker's displayed
 * qty back onto its price level (tail, as-if re-entered), the taker residual
 * routed by its TIF (GTC/GTD rest, IOC/FOK/MARKET cancel). The public feed
 * must equal the matching book after any hold/accept/reject sequence. Holds
 * are owned by the maker account, expire on idle via tick()/TimeTick, resolve
 * deterministically on cancel-while-held, and never count as firm liquidity
 * for a FOK. lastLookWindowNs == 0 disables the feature entirely.
 *
 * clOrdId dedup: per-account, session-window; a resend of a used clientOrderId
 * rejects (DuplicateClientOrderId) whether the original is resting, filled or
 * canceled; accounts do not collide; replay rebuilds the index.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sequenced_shard.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg(int64_t llWindow = 1000, bool acceptOnTimeout = false)
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.lastLookWindowNs = llWindow;
  c.lastLookAcceptOnTimeout = acceptOnTimeout;
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
  template <class T>
  int count() const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (std::get_if<T>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  int trades() const { return count<venue::Trade>(); }
  const FillHeld* lastHeld() const
  {
    const FillHeld* out = nullptr;
    for (auto& e : ev)
    {
      if (auto* h = std::get_if<FillHeld>(&e))
      {
        out = h;
      }
    }
    return out;
  }
  const FillRejected* lastFillRejected() const
  {
    const FillRejected* out = nullptr;
    for (auto& e : ev)
    {
      if (auto* h = std::get_if<FillRejected>(&e))
      {
        out = h;
      }
    }
    return out;
  }
  bool sawReject(RejectReason r) const
  {
    for (auto& e : ev)
    {
      if (auto* j = std::get_if<OrderRejected>(&e); j != nullptr && j->reason == r)
      {
        return true;
      }
    }
    return false;
  }
  bool sawCancel(CancelReason r) const
  {
    for (auto& e : ev)
    {
      if (auto* c = std::get_if<OrderCanceled>(&e); c != nullptr && c->reason == r)
      {
        return true;
      }
    }
    return false;
  }
};

// Displayed qty resting at a price on one side of the matching book.
Quantity bookAt(const MatchingBook& b, Side s, double p)
{
  std::vector<std::pair<Price, Quantity>> lv;
  b.levels(s, lv);
  for (const auto& [price, q] : lv)
  {
    if (price == px(p))
    {
      return q;
    }
  }
  return Quantity{};
}

// ---- pro-rata guard: lastLook orders are refused at admission ---------------

void test_prorata_lastlook_rejected()
{
  std::printf("test_prorata_lastlook_rejected\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink(), MatchingBook{}, MatchPolicy::ProRata);
  NewOrder ll = limit(1, Side::SELL, 100, 5, 1);
  ll.lastLook = true;
  eng.submit(InboundCommand{ll}, 1);
  CHECK(cap.sawReject(RejectReason::LastLookUnsupported));
  CHECK(bookAt(eng.book(), Side::SELL, 100).isZero());

  // Firm order on the same pro-rata instrument is fine.
  NewOrder firm = limit(2, Side::SELL, 100, 5, 1);
  eng.submit(InboundCommand{firm}, 2);
  CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(5));

  // Same lastLook order on a price-time instrument is fine.
  Cap cap2;
  MatchingEngine<MatchingBook> ptEng(cfg(), cap2.sink());
  NewOrder ok = limit(3, Side::SELL, 100, 5, 1);
  ok.lastLook = true;
  ptEng.submit(InboundCommand{ok}, 1);
  CHECK(bookAt(ptEng.book(), Side::SELL, 100) == qty(5));
}

// ---- T016: reject restores the book ----------------------------------------

void test_reject_restores_book()
{
  std::printf("test_reject_restores_book\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
  CHECK(cap.trades() == 0);
  CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(2));  // 3 held out of the book
  const auto* h = cap.lastHeld();
  CHECK(h != nullptr && h->qty == qty(3) && h->makerDisplayAfter == qty(2));
  eng.submit(InboundCommand{LastLookDecision{h->heldId, SYM, false, 1}}, 2);
  CHECK(cap.trades() == 0);
  // Maker's 3 are back on its level (tail); the GTC taker residual rests too.
  CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(5));
  CHECK(bookAt(eng.book(), Side::BUY, 100) == qty(3));
  const auto* fr = cap.lastFillRejected();
  CHECK(fr != nullptr && fr->qty == qty(3) && fr->makerId == 1 && fr->takerId == 2 &&
        fr->price == px(100));
  // The taker fully exited with zero fills before the reject; the restore is
  // what made it whole -- resend of either id must be a duplicate while resting.
  eng.submit(InboundCommand{limit(1, Side::SELL, 101, 1, 1)}, 3);
  CHECK(cap.sawReject(RejectReason::DuplicateOrderId));
}

// A mass quote can be marked non-firm, and both of its legs are.
//
// Holding a fill is how a maker protects a tight quote, and quoting
// continuously is exactly when a quote is tight. Before the flag reached the
// legs a maker had to choose between the primitive built for two-sided quoting
// and the control that makes two-sided quoting safe.
void test_quote_carries_lastlook()
{
  std::printf("test_quote_carries_lastlook\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  Quote q;
  q.bidId = 1;
  q.askId = 2;
  q.symbol = SYM;
  q.bidPrice = px(99);
  q.bidQty = qty(5);
  q.askPrice = px(101);
  q.askQty = qty(5);
  q.accountId = 1;
  q.lastLook = true;
  eng.submit(InboundCommand{q}, 0);

  // Hitting the ask leg holds instead of printing: the leg is non-firm. IOC so
  // the residual does not rest and get in the way of the second aggressor.
  NewOrder t1 = limit(3, Side::BUY, 101, 2, 2);
  t1.tif = TimeInForce::IOC;
  eng.submit(InboundCommand{t1}, 1);
  CHECK(cap.trades() == 0);
  const auto* h = cap.lastHeld();
  CHECK(h != nullptr && h->makerId == 2 && h->qty == qty(2));

  // Refused: the liquidity returns to the book rather than vanishing.
  eng.submit(InboundCommand{LastLookDecision{h->heldId, SYM, false, 1}}, 2);
  CHECK(cap.trades() == 0);
  CHECK(bookAt(eng.book(), Side::SELL, 101) == qty(5));

  // The bid leg is non-firm too -- both legs inherit it, not just the one the
  // first aggressor happened to hit.
  NewOrder t2 = limit(4, Side::SELL, 99, 2, 3);  // a different account, cleanly
  t2.tif = TimeInForce::IOC;
  eng.submit(InboundCommand{t2}, 3);
  CHECK(cap.trades() == 0);
  const auto* h2 = cap.lastHeld();
  CHECK(h2 != nullptr && h2->makerId == 1);
}

// The venue applies the price tolerance itself, on magnitude alone.
//
// A maker allowed to answer however it likes will, over enough holds, fill the
// ones that moved its way and refuse the ones that did not. The taker sees only
// a reject rate and cannot tell that from an honest wide tolerance. Enforcing
// the threshold at the venue leaves nothing to be asymmetric about outside the
// band -- a move too far is refused whoever it would have favoured.
void test_symmetric_price_tolerance()
{
  std::printf("test_symmetric_price_tolerance\n");

  // A move AGAINST the maker, beyond tolerance: refused even though the maker
  // said yes.
  {
    Cap cap;
    venue::SymbolConfig c = cfg();
    c.lastLookToleranceRaw = px(0.50).raw();
    MatchingEngine<MatchingBook> eng(c, cap.sink());
    // Establish a reference first: before the first trade there is nothing to
    // measure a move from, and the venue says so by not measuring one.
    eng.submit(InboundCommand{limit(90, Side::SELL, 100, 1, 8)}, 0);
    eng.submit(InboundCommand{limit(91, Side::BUY, 100, 1, 9)}, 0);
    // The whole quote is held, so nothing of the maker's is left on the book to
    // intercept the trades that move the price.
    NewOrder mk = limit(1, Side::SELL, 100, 3, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
    CHECK(cap.lastHeld() != nullptr);
    // Copied out now: lastHeld points into the capture vector, and the submits
    // below reallocate it.
    const uint64_t heldId = cap.lastHeld() ? cap.lastHeld()->heldId : 0;
    // The maker sold at 100; the market trades up to 102, so it is losing.
    eng.submit(InboundCommand{limit(3, Side::SELL, 102, 1, 3)}, 2);
    eng.submit(InboundCommand{limit(4, Side::BUY, 102, 1, 4)}, 3);
    eng.submit(InboundCommand{LastLookDecision{heldId, SYM, true, 1}}, 4);
    CHECK(eng.toleranceRejectedHolds() == 1);
  }

  // The SAME magnitude in the maker's favour is refused too. That is what
  // symmetric means, and it is the half a cherry-picking maker would keep.
  {
    Cap cap;
    venue::SymbolConfig c = cfg();
    c.lastLookToleranceRaw = px(0.50).raw();
    MatchingEngine<MatchingBook> eng(c, cap.sink());
    eng.submit(InboundCommand{limit(90, Side::SELL, 100, 1, 8)}, 0);
    eng.submit(InboundCommand{limit(91, Side::BUY, 100, 1, 9)}, 0);
    NewOrder mk = limit(1, Side::SELL, 100, 3, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
    CHECK(cap.lastHeld() != nullptr);
    const uint64_t heldId = cap.lastHeld() ? cap.lastHeld()->heldId : 0;
    // Down to 98: the maker sold at 100 and is now winning.
    eng.submit(InboundCommand{limit(3, Side::SELL, 98, 1, 3)}, 2);
    eng.submit(InboundCommand{limit(4, Side::BUY, 98, 1, 4)}, 3);
    eng.submit(InboundCommand{LastLookDecision{heldId, SYM, true, 1}}, 4);
    CHECK(eng.toleranceRejectedHolds() == 1);
  }

  // Inside the band the maker's answer still stands: the tolerance caps the
  // option, it does not abolish last look.
  {
    Cap cap;
    venue::SymbolConfig c = cfg();
    c.lastLookToleranceRaw = px(5.0).raw();
    MatchingEngine<MatchingBook> eng(c, cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
    const uint64_t heldId = cap.lastHeld() ? cap.lastHeld()->heldId : 0;
    eng.submit(InboundCommand{LastLookDecision{heldId, SYM, true, 1}}, 2);
    CHECK(cap.trades() == 1);
    CHECK(eng.toleranceRejectedHolds() == 0);
  }
}

// Conduct is measured by WHICH holds a maker refused, not how many.
//
// A maker with an honest wide tolerance and one taking the free option post the
// same reject rate. What separates them is the direction the price had moved
// when they said no.
void test_last_look_conduct_is_visible()
{
  std::printf("test_last_look_conduct_is_visible\n");
  Cap cap;
  venue::SymbolConfig c = cfg();
  MatchingEngine<MatchingBook> eng(c, cap.sink());
  eng.submit(InboundCommand{limit(90, Side::SELL, 100, 1, 8)}, 0);
  eng.submit(InboundCommand{limit(91, Side::BUY, 100, 1, 9)}, 0);

  int64_t ts = 0;
  // Run the same episode twice: once with the price moving against the maker,
  // once in its favour. The maker refuses only when it is losing.
  for (int round = 0; round < 2; ++round)
  {
    const bool adverse = (round == 0);
    NewOrder mk = limit(100 + round * 10, Side::SELL, 100, 1, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, ++ts);
    eng.submit(InboundCommand{limit(101 + round * 10, Side::BUY, 100, 1, 2)}, ++ts);
    CHECK(cap.lastHeld() != nullptr);
    const uint64_t heldId = cap.lastHeld() ? cap.lastHeld()->heldId : 0;
    const double to = adverse ? 102 : 98;
    eng.submit(InboundCommand{limit(102 + round * 10, Side::SELL, to, 1, 3)}, ++ts);
    eng.submit(InboundCommand{limit(103 + round * 10, Side::BUY, to, 1, 4)}, ++ts);
    eng.submit(InboundCommand{LastLookDecision{heldId, SYM, !adverse, 1}}, ++ts);
  }

  // The same on the other side of the book. A maker that BOUGHT is hurt by a
  // falling price, not a rising one -- so which move counts as adverse depends
  // on the side the maker took. Testing only one side leaves the sign
  // unverified, and a sign error there mislabels every maker on the bid.
  {
    NewOrder mkBid = limit(200, Side::BUY, 100, 1, 1);
    mkBid.lastLook = true;
    eng.submit(InboundCommand{mkBid}, ++ts);
    eng.submit(InboundCommand{limit(201, Side::SELL, 100, 1, 2)}, ++ts);
    CHECK(cap.lastHeld() != nullptr);
    const uint64_t heldId = cap.lastHeld() ? cap.lastHeld()->heldId : 0;
    // Down to 98: the maker bought at 100, so this is adverse for it.
    eng.submit(InboundCommand{limit(202, Side::SELL, 98, 1, 3)}, ++ts);
    eng.submit(InboundCommand{limit(203, Side::BUY, 98, 1, 4)}, ++ts);
    eng.submit(InboundCommand{LastLookDecision{heldId, SYM, false, 1}}, ++ts);
  }

  const auto& stats = eng.lastLookStats();
  auto it = stats.find(1);
  CHECK(it != stats.end());
  if (it != stats.end())
  {
    CHECK(it->second.held == 3);
    // Two adverse holds -- one where the maker sold into a rising market, one
    // where it bought into a falling one -- and it refused both.
    CHECK(it->second.adverse == 2 && it->second.rejectedAdverse == 2);
    CHECK(it->second.favourable == 1 && it->second.rejectedFavourable == 0);
  }
}

void test_partial_hold_reject()
{
  std::printf("test_partial_hold_reject\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 2, 3)}, 0);  // firm maker, acct 3
  NewOrder mk = limit(2, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 1);
  // Taker 6: fills 2 firm (trade), holds 4 from the last-look maker, leaves 0.
  eng.submit(InboundCommand{limit(3, Side::BUY, 100, 6, 2)}, 2);
  CHECK(cap.trades() == 1);
  const auto* h = cap.lastHeld();
  CHECK(h != nullptr && h->qty == qty(4));
  eng.submit(InboundCommand{LastLookDecision{h->heldId, SYM, false, 1}}, 3);
  CHECK(cap.trades() == 1);                              // no new prints
  CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(5));  // maker fully restored
  CHECK(bookAt(eng.book(), Side::BUY, 100) == qty(4));   // held taker slice rests
}

// ---- T016: taker residual per TIF -------------------------------------------

void test_taker_residual_tifs()
{
  std::printf("test_taker_residual_tifs\n");
  {  // GTC: residual rests (covered above too; assert no cancel event)
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
    eng.submit(InboundCommand{LastLookDecision{cap.lastHeld()->heldId, SYM, false, 1}}, 2);
    CHECK(bookAt(eng.book(), Side::BUY, 100) == qty(3));
    CHECK(cap.count<OrderCanceled>() == 0);
  }
  {  // IOC: residual canceled with the IOC reason, nothing rests
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    NewOrder tk = limit(2, Side::BUY, 100, 3, 2);
    tk.tif = TimeInForce::IOC;
    eng.submit(InboundCommand{tk}, 1);
    eng.submit(InboundCommand{LastLookDecision{cap.lastHeld()->heldId, SYM, false, 1}}, 2);
    CHECK(cap.sawCancel(CancelReason::ImmediateOrCancelResidual));
    CHECK(bookAt(eng.book(), Side::BUY, 100).isZero());
    CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(5));  // maker still restored
  }
  {  // MARKET: residual canceled with the market reason
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    NewOrder tk;
    tk.id = 2;
    tk.symbol = SYM;
    tk.side = Side::BUY;
    tk.type = OrderType::MARKET;
    tk.quantity = qty(3);
    tk.accountId = 2;
    eng.submit(InboundCommand{tk}, 1);
    const auto* h = cap.lastHeld();
    CHECK(h != nullptr && h->qty == qty(3));
    eng.submit(InboundCommand{LastLookDecision{h->heldId, SYM, false, 1}}, 2);
    CHECK(cap.sawCancel(CancelReason::MarketResidual));
    CHECK(bookAt(eng.book(), Side::BUY, 100).isZero());
  }
}

// ---- T016: public feed == matching book -------------------------------------

void test_md_equals_book()
{
  std::printf("test_md_equals_book\n");
  std::vector<MdMessage> md;
  MarketDataPublisher<1 << 16> pub([&](const MdMessage& m)
                                   { md.push_back(m); }, px(0.01), SYM);
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     cap.ev.push_back(e);
                                     pub.onEvent(e, eng.engineTimeNs()); });

  auto feedMatchesBook = [&](std::vector<double> prices)
  {
    for (double p : prices)
    {
      if (pub.book().bidAtPrice(px(p)).raw() != bookAt(eng.book(), Side::BUY, p).raw())
      {
        return false;
      }
      if (pub.book().askAtPrice(px(p)).raw() != bookAt(eng.book(), Side::SELL, p).raw())
      {
        return false;
      }
    }
    return true;
  };
  const std::vector<double> prices{99.0, 100.0, 101.0};

  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);
  CHECK(feedMatchesBook(prices));

  // Hold: the feed must drop the held size immediately (no phantom liquidity).
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
  CHECK(pub.book().askAtPrice(px(100)) == qty(2));
  CHECK(feedMatchesBook(prices));

  // Reject: maker restored, taker residual rests -- feed follows both.
  eng.submit(InboundCommand{LastLookDecision{cap.lastHeld()->heldId, SYM, false, 1}}, 2);
  CHECK(pub.book().askAtPrice(px(100)) == qty(5));
  CHECK(pub.book().bidAtPrice(px(100)) == qty(3));
  CHECK(feedMatchesBook(prices));

  // Hold + accept: depth already left at hold time; the print must not change it.
  NewOrder tk = limit(3, Side::BUY, 100, 2, 3);
  tk.tif = TimeInForce::IOC;
  eng.submit(InboundCommand{tk}, 3);
  CHECK(feedMatchesBook(prices));
  eng.submit(InboundCommand{LastLookDecision{cap.lastHeld()->heldId, SYM, true, 1}}, 4);
  CHECK(cap.trades() == 1);
  CHECK(pub.book().askAtPrice(px(100)) == qty(3));
  CHECK(feedMatchesBook(prices));

  // Cancel-while-held: resolve + cancel must drain both feed and book.
  eng.submit(InboundCommand{limit(4, Side::BUY, 100, 1, 3)}, 5);  // new hold (1 of maker's 3)
  CHECK(cap.count<FillHeld>() == 3);
  eng.submit(InboundCommand{CancelOrder{1, SYM, 1}}, 6);  // maker cancels with a live hold
  eng.submit(InboundCommand{CancelOrder{2, SYM, 2}}, 7);
  eng.submit(InboundCommand{CancelOrder{4, SYM, 3}}, 8);
  CHECK(eng.book().empty());
  CHECK(feedMatchesBook(prices));
}

// ---- T016: ownership --------------------------------------------------------

void test_ownership()
{
  std::printf("test_ownership\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
  const auto* h = cap.lastHeld();
  CHECK(h != nullptr);
  const uint64_t heldId = h->heldId;  // copy: cap.ev may reallocate below
  // The taker (or anyone but the maker) must not be able to accept its own fill.
  eng.submit(InboundCommand{LastLookDecision{heldId, SYM, true, 2}}, 2);
  CHECK(cap.sawReject(RejectReason::NotOrderOwner));
  CHECK(cap.trades() == 0);  // the hold is untouched
  // The real maker still can.
  eng.submit(InboundCommand{LastLookDecision{heldId, SYM, true, 1}}, 3);
  CHECK(cap.trades() == 1);
}

// ---- T016: timeout accept, idle expiry, window=0 ----------------------------

void test_timeout_accept()
{
  std::printf("test_timeout_accept\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(1000, /*acceptOnTimeout*/ true), cap.sink());
  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // deadline 1001
  CHECK(cap.trades() == 0);
  eng.submit(InboundCommand{CancelOrder{999, SYM, 9}}, 5000);  // time passes -> accept
  CHECK(cap.trades() == 1);
  CHECK(cap.count<FillRejected>() == 0);
  CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(2));
}

void test_idle_expiry_via_tick()
{
  std::printf("test_idle_expiry_via_tick\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // deadline 1001
  CHECK(eng.openHolds() == 1);
  eng.tick(500);  // before the deadline: nothing happens
  CHECK(eng.openHolds() == 1);
  eng.tick(5000);  // NO traffic -- the sweep alone expires the hold (reject)
  CHECK(eng.openHolds() == 0);
  CHECK(cap.count<FillRejected>() == 1);
  CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(5));
  CHECK(bookAt(eng.book(), Side::BUY, 100) == qty(3));
  eng.tick(6000);  // idempotent
  CHECK(cap.count<FillRejected>() == 1);
}

void test_window_zero_disables()
{
  std::printf("test_window_zero_disables\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(/*window*/ 0), cap.sink());
  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;  // flag set, but the venue has last look OFF
  eng.submit(InboundCommand{mk}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
  CHECK(cap.count<FillHeld>() == 0);
  CHECK(cap.trades() == 1);  // fills like a normal maker
}

// ---- T016: cancel-while-held + conservation ---------------------------------

void test_cancel_while_held_conservation()
{
  std::printf("test_cancel_while_held_conservation\n");
  constexpr AssetId BASE = 0, QUOTE = 1;
  constexpr uint64_t VENUE = 999;
  const Amount base5 = amountOf(qty(5));
  const Amount usd300 = amountOf(Volume::fromDouble(300));
  Ledger led;
  led.deposit(1, BASE, base5);
  led.deposit(2, QUOTE, usd300);
  auto c = cfg();
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  Cap cap;
  MatchingEngine<MatchingBook> eng(c, cap.sink());
  eng.setLedger(&led, VENUE);
  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
  const auto* h = cap.lastHeld();
  CHECK(h != nullptr);
  const uint64_t heldId = h->heldId;

  // Maker cancels its order while the hold is live: the hold resolves (reject)
  // FIRST, deterministically, then the whole order cancels.
  eng.submit(InboundCommand{CancelOrder{1, SYM, 1}}, 2);
  CHECK(cap.count<FillRejected>() == 1);
  CHECK(cap.sawCancel(CancelReason::UserRequested));
  CHECK(bookAt(eng.book(), Side::SELL, 100).isZero());
  // Maker made whole immediately; taker's restored GTC residual rests, backed
  // by its reservation.
  CHECK(led.available(1, BASE) == base5 && led.reserved(1, BASE) == 0);
  CHECK(bookAt(eng.book(), Side::BUY, 100) == qty(3));
  CHECK(led.reserved(2, QUOTE) == usd300);

  // Accept-after-cancel must be impossible.
  eng.submit(InboundCommand{LastLookDecision{heldId, SYM, true, 1}}, 3);
  CHECK(cap.trades() == 0);
  CHECK(cap.sawReject(RejectReason::UnknownOrder));

  // Conservation, then a full drain returns every reserved unit.
  CHECK(led.total(1, BASE) == base5 && led.total(2, QUOTE) == usd300);
  eng.submit(InboundCommand{CancelOrder{2, SYM, 2}}, 4);
  CHECK(led.available(2, QUOTE) == usd300 && led.reserved(2, QUOTE) == 0);
}

// ---- T028: STP-cancel of a held maker ---------------------------------------

// Self-trade prevention removes a resting maker from INSIDE the matcher, which
// is the one cancel path that never went through the engine's hold discipline.
// A last-look maker with displayed size reaches that path: STP cancels it, the
// cancel frees the collateral that its OPEN hold still needs, and the later
// accept settles with no reservation behind it -- the branch where the debit's
// result was discarded and the counterparty credited anyway (value minted).
// The STP path must resolve the maker's holds FIRST, exactly like every other
// removal.
void test_stp_cancel_while_held_conservation()
{
  std::printf("test_stp_cancel_while_held_conservation\n");
  constexpr AssetId BASE = 0, QUOTE = 1;
  constexpr uint64_t VENUE = 999;
  const Amount base5 = amountOf(qty(5));
  const Amount usd300 = amountOf(Volume::fromDouble(300));
  Ledger led;
  led.deposit(1, BASE, base5);    // maker: exactly the 5 base it quotes
  led.deposit(2, QUOTE, usd300);  // taker
  led.deposit(1, QUOTE, usd300);  // maker's STP order needs quote to reserve
  auto c = cfg();
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  Cap cap;
  MatchingEngine<MatchingBook> eng(c, cap.sink());
  eng.setLedger(&led, VENUE);
  const Amount initBase = led.total(1, BASE) + led.total(2, BASE) + led.total(VENUE, BASE);
  const Amount initQuote = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);

  NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);
  CHECK(led.reserved(1, BASE) == base5);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // holds 3 of the 5
  const auto* h = cap.lastHeld();
  CHECK(h != nullptr);
  const uint64_t heldId = h != nullptr ? h->heldId : 0;
  CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(2));  // 3 held out of the book

  // The maker's OWN account crosses its own quote with self-trade prevention:
  // the resting (held) maker is pulled from inside the matcher.
  NewOrder stp = limit(3, Side::BUY, 100, 2, 1);
  stp.stp = STPMode::CancelOldest;
  eng.submit(InboundCommand{stp}, 2);
  CHECK(cap.sawCancel(CancelReason::SelfTradePrevention));
  CHECK(cap.count<FillRejected>() == 1);  // the hold was resolved before the cancel
  CHECK(bookAt(eng.book(), Side::SELL, 100).isZero());
  // Maker made whole: the full 5 base is back in `available`, nothing stranded.
  CHECK(led.available(1, BASE) == base5 && led.reserved(1, BASE) == 0);

  // The maker now spends the freed collateral elsewhere. If the STP cancel had
  // stripped a live hold's backing, the accept below would settle from an empty
  // account and print base that does not exist.
  eng.submit(InboundCommand{Withdraw{1, BASE, static_cast<int64_t>(base5), SYM}}, 3);
  CHECK(led.available(1, BASE) == 0);

  eng.submit(InboundCommand{LastLookDecision{heldId, SYM, true, 1}}, 4);
  CHECK(cap.trades() == 0);                          // no fill can settle from a resolved hold
  CHECK(cap.sawReject(RejectReason::UnknownOrder));  // the heldId is gone for good
  CHECK(eng.unsettledTrades() == 0);                 // and nothing reached the no-reservation path

  // Conservation: base only left through the withdrawal, quote never moved.
  const Amount endBase = led.total(1, BASE) + led.total(2, BASE) + led.total(VENUE, BASE);
  const Amount endQuote = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(endBase == initBase - base5);
  CHECK(endQuote == initQuote);

  // Book and tracking agree: the STP-canceled maker is gone from both, and the
  // live orders are exactly the taker's restored residual plus the STP
  // aggressor's own (uncrossed) remainder.
  CHECK(!eng.book().contains(1));
  CHECK(eng.restingOrderCount() == 2);
  eng.submit(InboundCommand{CancelOrder{2, SYM, 2}}, 5);
  eng.submit(InboundCommand{CancelOrder{3, SYM, 1}}, 6);
  CHECK(eng.restingOrderCount() == 0);
  CHECK(led.reserved(2, QUOTE) == 0 && led.available(2, QUOTE) == usd300);
  CHECK(led.reserved(1, QUOTE) == 0 && led.available(1, QUOTE) == usd300);
}

// ---- T016: FOK vs last-look -------------------------------------------------

void test_fok_vs_lastlook()
{
  std::printf("test_fok_vs_lastlook\n");
  {  // crossable liquidity is last-look only -> FOK rejected, book untouched
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    NewOrder tk = limit(2, Side::BUY, 100, 3, 2);
    tk.tif = TimeInForce::FOK;
    eng.submit(InboundCommand{tk}, 1);
    CHECK(cap.sawReject(RejectReason::FillOrKillUnfulfillable));
    CHECK(cap.count<FillHeld>() == 0);
    CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(5));
  }
  {  // enough firm size, but a last-look maker inside the crossing range -> reject
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    NewOrder ll = limit(1, Side::SELL, 100, 5, 1);
    ll.lastLook = true;
    eng.submit(InboundCommand{ll}, 0);
    eng.submit(InboundCommand{limit(2, Side::SELL, 101, 5, 3)}, 1);  // firm
    NewOrder tk = limit(3, Side::BUY, 101, 5, 2);
    tk.tif = TimeInForce::FOK;
    eng.submit(InboundCommand{tk}, 2);
    CHECK(cap.sawReject(RejectReason::FillOrKillUnfulfillable));
    CHECK(cap.trades() == 0 && cap.count<FillHeld>() == 0);
  }
  {  // last-look maker OUTSIDE the crossing range: FOK executes normally
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    NewOrder ll = limit(1, Side::SELL, 102, 5, 1);
    ll.lastLook = true;
    eng.submit(InboundCommand{ll}, 0);
    eng.submit(InboundCommand{limit(2, Side::SELL, 101, 5, 3)}, 1);  // firm
    NewOrder tk = limit(3, Side::BUY, 101, 5, 2);
    tk.tif = TimeInForce::FOK;
    eng.submit(InboundCommand{tk}, 2);
    CHECK(cap.trades() == 1);
    CHECK(cap.count<FillHeld>() == 0);
  }
}

// ---- T016: idle sweeper on the sequenced shard (journaled TimeTick) ---------

void test_shard_idle_sweeper()
{
  std::printf("test_shard_idle_sweeper\n");
  const std::string path = "/tmp/flox_test_venue_lastlook_sweeper.bin";
  std::remove(path.c_str());

  auto now = std::make_shared<std::atomic<int64_t>>(1000);
  SequencedShard<>::TimeSource clk = [now]
  { return now->load(); };

  std::atomic<int> fillHeld{0};
  std::atomic<int> fillRejected{0};
  struct Sink : IEngineEventListener
  {
    std::atomic<int>* held{nullptr};
    std::atomic<int>* rejected{nullptr};
    void onEngineEvent(const EngineEventMsg& e) override
    {
      if (std::get_if<FillHeld>(&e.event))
      {
        held->fetch_add(1);
      }
      if (std::get_if<FillRejected>(&e.event))
      {
        rejected->fetch_add(1);
      }
    }
  } sink;
  sink.held = &fillHeld;
  sink.rejected = &fillRejected;

  {
    auto shard = std::make_unique<SequencedShard<>>(cfg(/*window*/ 5000), path, MatchingBook{},
                                                    Journal::Sync::Off, clk,
                                                    /*idleSweepIntervalNs*/ 1000);
    shard->subscribeOutbound(&sink);
    shard->start();
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    shard->submit(InboundCommand{mk});
    shard->submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)});
    shard->flush();
    CHECK(fillHeld.load() == 1);
    CHECK(fillRejected.load() == 0);

    // NO further traffic. Advance the injected clock past the window; the idle
    // sweeper must inject a TimeTick that expires the hold.
    now->store(1'000'000);
    for (int i = 0; i < 2000 && fillRejected.load() == 0; ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(fillRejected.load() == 1);
    shard->stop();
  }

  // The sweep went through the journal: replaying it reproduces the timeout.
  const auto records = Journal::loadTimed(path);
  int ticks = 0;
  for (const auto& [ts, cmd] : records)
  {
    (void)ts;
    if (std::get_if<TimeTick>(&cmd))
    {
      ++ticks;
    }
  }
  CHECK(ticks >= 1);
  int replayRejected = 0;
  MatchingEngine<MatchingBook> rec(cfg(5000), [&](const OutboundEvent& e)
                                   {
                                     if (std::get_if<FillRejected>(&e))
                                     {
                                       ++replayRejected;
                                     } });
  for (const auto& [ts, cmd] : records)
  {
    rec.submit(cmd, ts);
  }
  CHECK(replayRejected == 1);
  std::remove(path.c_str());
}

// ---- T020: clientOrderId dedup ----------------------------------------------

void test_clordid_dedup()
{
  std::printf("test_clordid_dedup\n");
  {  // resend after the original FILLED -> reject, no second execution
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(0), cap.sink());
    eng.submit(InboundCommand{limit(1, Side::SELL, 100, 3, 1)}, 0);
    NewOrder tk = limit(2, Side::BUY, 100, 3, 2);
    tk.clientOrderId = 77;
    eng.submit(InboundCommand{tk}, 1);
    CHECK(cap.trades() == 1);
    NewOrder resend = limit(3, Side::BUY, 100, 3, 2);  // fresh venue id, same clOrdId
    resend.clientOrderId = 77;
    eng.submit(InboundCommand{resend}, 2);
    CHECK(cap.trades() == 1);  // did NOT execute twice
    CHECK(cap.sawReject(RejectReason::DuplicateClientOrderId));
    CHECK(eng.book().empty());  // book untouched by the resend
  }
  {  // resend while the original still RESTS -> reject, original unharmed
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(0), cap.sink());
    NewOrder o = limit(10, Side::SELL, 100, 3, 1);
    o.clientOrderId = 88;
    eng.submit(InboundCommand{o}, 0);
    NewOrder resend = limit(11, Side::SELL, 100, 3, 1);
    resend.clientOrderId = 88;
    eng.submit(InboundCommand{resend}, 1);
    CHECK(cap.sawReject(RejectReason::DuplicateClientOrderId));
    CHECK(bookAt(eng.book(), Side::SELL, 100) == qty(3));  // only the original
  }
  {  // resend after cancel -> still a duplicate (session window)
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(0), cap.sink());
    NewOrder o = limit(10, Side::SELL, 100, 3, 1);
    o.clientOrderId = 88;
    eng.submit(InboundCommand{o}, 0);
    eng.submit(InboundCommand{CancelOrder{10, SYM, 1}}, 1);
    NewOrder resend = limit(11, Side::SELL, 100, 3, 1);
    resend.clientOrderId = 88;
    eng.submit(InboundCommand{resend}, 2);
    CHECK(cap.sawReject(RejectReason::DuplicateClientOrderId));
  }
  {  // dedup is per account: two accounts may use the same clientOrderId
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(0), cap.sink());
    NewOrder a = limit(20, Side::SELL, 101, 1, 1);
    a.clientOrderId = 99;
    NewOrder b = limit(21, Side::SELL, 102, 1, 2);
    b.clientOrderId = 99;
    eng.submit(InboundCommand{a}, 0);
    eng.submit(InboundCommand{b}, 1);
    CHECK(cap.count<OrderAccepted>() == 2);
    CHECK(cap.count<OrderRejected>() == 0);
  }
  {  // clientOrderId 0 = unset: never deduplicated
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(0), cap.sink());
    eng.submit(InboundCommand{limit(30, Side::SELL, 101, 1, 1)}, 0);
    eng.submit(InboundCommand{limit(31, Side::SELL, 102, 1, 1)}, 1);
    CHECK(cap.count<OrderAccepted>() == 2);
  }
}

// Replay safety: the dedup index is rebuilt by the same submits during journal
// replay, so a post-restart resend behaves exactly like a live one.
void test_clordid_dedup_survives_replay()
{
  std::printf("test_clordid_dedup_survives_replay\n");
  const std::string path = "/tmp/flox_test_venue_lastlook_clordid.bin";
  std::remove(path.c_str());

  struct Sink : IEngineEventListener
  {
    std::atomic<int> trades{0};
    std::atomic<int> dupRejects{0};
    void onEngineEvent(const EngineEventMsg& e) override
    {
      if (std::get_if<venue::Trade>(&e.event))
      {
        trades.fetch_add(1);
      }
      if (auto* j = std::get_if<OrderRejected>(&e.event);
          j != nullptr && j->reason == RejectReason::DuplicateClientOrderId)
      {
        dupRejects.fetch_add(1);
      }
    }
  };

  {  // live session: submit + fill with clOrdId 55
    Sink sink;
    auto shard = std::make_unique<SequencedShard<>>(cfg(0), path, MatchingBook{},
                                                    Journal::Sync::Off);
    shard->subscribeOutbound(&sink);
    shard->start();
    shard->submit(InboundCommand{limit(1, Side::SELL, 100, 3, 1)});
    NewOrder tk = limit(2, Side::BUY, 100, 3, 2);
    tk.clientOrderId = 55;
    shard->submit(InboundCommand{tk});
    shard->flush();
    shard->stop();
    CHECK(sink.trades.load() == 1);
  }
  {  // restart: recovery replays the journal, then the resend must reject
    Sink sink;
    auto shard = std::make_unique<SequencedShard<>>(cfg(0), path, MatchingBook{},
                                                    Journal::Sync::Off);
    shard->subscribeOutbound(&sink);
    shard->start();
    CHECK(shard->recoveredCommands() == 2u);
    NewOrder resend = limit(3, Side::BUY, 100, 3, 2);
    resend.clientOrderId = 55;
    shard->submit(InboundCommand{resend});
    shard->flush();
    shard->stop();
    CHECK(sink.trades.load() == 0);  // recovery does not re-broadcast, resend did not trade
    CHECK(sink.dupRejects.load() == 1);
  }
  std::remove(path.c_str());
}

}  // namespace

TEST(VenueLastLook, LifecycleSuite)
{
  test_prorata_lastlook_rejected();
  test_reject_restores_book();
  test_quote_carries_lastlook();
  test_symmetric_price_tolerance();
  test_last_look_conduct_is_visible();
  test_partial_hold_reject();
  test_taker_residual_tifs();
  test_md_equals_book();
  test_ownership();
  test_timeout_accept();
  test_idle_expiry_via_tick();
  test_window_zero_disables();
  test_cancel_while_held_conservation();
  test_stp_cancel_while_held_conservation();
  test_fok_vs_lastlook();
  test_shard_idle_sweeper();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}

TEST(VenueClientOrderId, DedupSuite)
{
  const int before = g_failures;
  test_clordid_dedup();
  test_clordid_dedup_survives_replay();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, before);
}
