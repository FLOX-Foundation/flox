/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A maker that behaves the way you tell it to.
 *
 * Someone writing a client against a last-look venue needs the other side to
 * misbehave on demand: accept everything, refuse everything, refuse only when
 * the market moved against it, refuse only when it moved its way, or say
 * nothing at all and let the window run out. The venue can already produce
 * every one of those; what was missing was something to drive it.
 *
 * This is that. It is not a mock: the decisions it returns are real
 * LastLookDecision commands and the venue applies them through the same path
 * live traffic uses, so what a client sees here is what it will see in
 * production.
 *
 * Deciding is a separate call from observing, on purpose. A maker that
 * answered inside onEvent would answer before the market had a chance to
 * move, and the one scenario worth detecting -- refusing only the fills that
 * moved its way -- is defined by what happened during the window. The driver
 * calls decide() when it is ready, which is also what makes a test of this
 * deterministic rather than a race.
 *
 * Engine-side: no sockets. It works against an engine in-process or behind a
 * gateway, whichever the client author needs.
 */
#pragma once

#include "flox-venue/messages.h"

#include <cstdint>
#include <vector>

namespace flox::venue::script
{

enum class MakerPolicy : uint8_t
{
  AcceptAlways,
  RejectAlways,
  // Refuses when the price moved AGAINST it during the window. This is a
  // maker managing real risk: it is being asked to honour a price the market
  // has left behind.
  RejectOnAdverse,
  // Refuses when the price moved ITS WAY. This is the free option -- it keeps
  // the fills that turned out well and hands back the ones that did not --
  // and it is the behaviour fme_last_look_rejects_favourable_total exists to
  // make visible.
  RejectOnFavourable,
  // Never answers. The venue's window expires and resolves the hold by its
  // own rule (lastLookAcceptOnTimeout).
  Silent,
};

class ScriptedMaker
{
 public:
  ScriptedMaker(uint64_t account, MakerPolicy policy) : account_(account), policy_(policy) {}

  // Every outbound event the venue produced. Holds are remembered; prints
  // move the reference the decision will be measured against.
  void observe(const OutboundEvent& e)
  {
    if (const auto* t = std::get_if<Trade>(&e))
    {
      lastPriceRaw_ = t->price.raw();
      havePrice_ = true;
      return;
    }
    if (const auto* h = std::get_if<FillHeld>(&e))
    {
      if (h->makerAccount != account_)
      {
        return;  // somebody else's hold
      }
      // The maker's side is the opposite of the aggressor's, and the hold
      // carries the aggressor's. This used to be told to the maker order by
      // order through a placed() call, because the report did not say: a map
      // of sides that grew with every quote, and a hold whose maker was not
      // in it could not be judged at all.
      const Side makerSide = h->takerSide == Side::BUY ? Side::SELL : Side::BUY;
      open_.push_back(Hold{h->heldId, h->makerId, h->price.raw(), makerSide});
    }
  }

  // Answer every hold that is still open, per the policy. Returns the
  // commands to submit; the caller decides when.
  void decide(std::vector<InboundCommand>& out)
  {
    if (policy_ == MakerPolicy::Silent)
    {
      open_.clear();  // it will time out at the venue; nothing to send
      return;
    }
    for (const Hold& h : open_)
    {
      bool accept = true;
      switch (policy_)
      {
        case MakerPolicy::AcceptAlways:
          accept = true;
          break;
        case MakerPolicy::RejectAlways:
          accept = false;
          break;
        case MakerPolicy::RejectOnAdverse:
          accept = !movedAgainst(h);
          break;
        case MakerPolicy::RejectOnFavourable:
          accept = movedAgainst(h) || !moved(h);
          break;
        case MakerPolicy::Silent:
          break;
      }
      LastLookDecision d;
      d.heldId = h.heldId;
      d.symbol = symbol_;
      d.accept = accept;
      d.accountId = account_;
      out.push_back(InboundCommand{d});
      ++decisions_;
      accepts_ += accept ? 1 : 0;
    }
    open_.clear();
  }

  void setSymbol(SymbolId s) { symbol_ = s; }

  uint64_t decisions() const noexcept { return decisions_; }
  uint64_t accepts() const noexcept { return accepts_; }
  size_t openHolds() const noexcept { return open_.size(); }

 private:
  struct Hold
  {
    uint64_t heldId{};
    OrderId makerId{};
    int64_t heldPriceRaw{};
    Side makerSide{};
  };

  bool moved(const Hold& h) const { return havePrice_ && lastPriceRaw_ != h.heldPriceRaw; }

  // Against the maker: a seller is hurt when the price rises, a buyer when it
  // falls. Without a print since the hold there is no move to judge, and an
  // unjudged move is not an adverse one.
  bool movedAgainst(const Hold& h) const
  {
    if (!moved(h))
    {
      return false;
    }
    return h.makerSide == Side::SELL ? lastPriceRaw_ > h.heldPriceRaw
                                     : lastPriceRaw_ < h.heldPriceRaw;
  }

  uint64_t account_{};
  MakerPolicy policy_{MakerPolicy::AcceptAlways};
  SymbolId symbol_{1};
  std::vector<Hold> open_;
  int64_t lastPriceRaw_{0};
  bool havePrice_{false};
  uint64_t decisions_{0};
  uint64_t accepts_{0};
};

}  // namespace flox::venue::script
