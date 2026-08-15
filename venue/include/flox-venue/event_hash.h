/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include <cstdint>

namespace flox::venue
{

inline uint64_t mix(uint64_t h, uint64_t v) noexcept
{
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

// Fold an outbound event into the determinism digest. Every field that can
// differ between two legal runs and that affects settlement, risk, or a
// client-visible report is mixed in -- so a real divergence (e.g. flipped taker
// side, a fill at a different price, ADL vs a plain force-close) changes the
// hash and is caught by the reproducibility/differential tests. The ONLY things
// intentionally excluded are values not carried by the event itself (wall-clock,
// pointers); if a field is on the struct, it is hashed.
inline uint64_t hashEvent(uint64_t h, const OutboundEvent& e) noexcept
{
  if (const auto* x = std::get_if<OrderAccepted>(&e))
  {
    h = mix(h, 1);
    h = mix(h, x->id);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->side));
    h = mix(h, static_cast<uint64_t>(x->price.raw()));
    h = mix(h, static_cast<uint64_t>(x->leavesQty.raw()));
    h = mix(h, x->restingOnBook ? 1U : 0U);
    h = mix(h, static_cast<uint64_t>(x->displayQty.raw()));
    h = mix(h, x->account);
  }
  else if (const auto* x = std::get_if<OrderRejected>(&e))
  {
    h = mix(h, 2);
    h = mix(h, x->id);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->reason));
    h = mix(h, x->account);
  }
  else if (const auto* x = std::get_if<CancelRejected>(&e))
  {
    h = mix(h, 0xC17U);
    h = mix(h, static_cast<uint64_t>(x->id));
    h = mix(h, static_cast<uint64_t>(x->symbol));
    h = mix(h, static_cast<uint64_t>(x->reason));
    h = mix(h, x->account);
    h = mix(h, x->wasReplace ? 1U : 0U);
  }
  else if (const auto* x = std::get_if<Trade>(&e))
  {
    h = mix(h, 3);
    h = mix(h, x->tradeId);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->price.raw()));
    h = mix(h, static_cast<uint64_t>(x->quantity.raw()));
    h = mix(h, x->makerId);
    h = mix(h, x->takerId);
    h = mix(h, static_cast<uint64_t>(x->takerSide));
    h = mix(h, x->makerAccount);
    h = mix(h, x->takerAccount);
  }
  else if (const auto* x = std::get_if<OrderExecuted>(&e))
  {
    h = mix(h, 4);
    h = mix(h, x->id);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->lastQty.raw()));
    h = mix(h, static_cast<uint64_t>(x->leavesQty.raw()));
    h = mix(h, x->aggressor ? 1U : 0U);
    h = mix(h, x->complete ? 1U : 0U);
    h = mix(h, static_cast<uint64_t>(x->lastPx.raw()));
    h = mix(h, static_cast<uint64_t>(x->displayLeaves.raw()));
    h = mix(h, x->account);
  }
  else if (const auto* x = std::get_if<OrderCanceled>(&e))
  {
    h = mix(h, 5);
    h = mix(h, x->id);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->reason));
    h = mix(h, x->account);
  }
  else if (const auto* x = std::get_if<OrderModified>(&e))
  {
    h = mix(h, 6);
    h = mix(h, x->id);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->price.raw()));
    h = mix(h, static_cast<uint64_t>(x->leavesQty.raw()));
    h = mix(h, x->priorityKept ? 1U : 0U);
    h = mix(h, x->account);
  }
  else if (const auto* x = std::get_if<OrderTriggered>(&e))
  {
    h = mix(h, 7);
    h = mix(h, x->id);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->refPrice.raw()));
    h = mix(h, x->account);
  }
  else if (const auto* x = std::get_if<FillHeld>(&e))
  {
    h = mix(h, 8);
    h = mix(h, x->heldId);
    h = mix(h, x->symbol);
    h = mix(h, x->makerId);
    h = mix(h, x->takerId);
    h = mix(h, static_cast<uint64_t>(x->price.raw()));
    h = mix(h, static_cast<uint64_t>(x->qty.raw()));
    h = mix(h, static_cast<uint64_t>(x->makerDisplayAfter.raw()));
    h = mix(h, x->makerAccount);
    h = mix(h, x->takerAccount);
  }
  else if (const auto* x = std::get_if<FillRejected>(&e))
  {
    h = mix(h, 9);
    h = mix(h, x->heldId);
    h = mix(h, x->symbol);
    h = mix(h, x->takerId);
    h = mix(h, x->makerId);
    h = mix(h, static_cast<uint64_t>(x->price.raw()));
    h = mix(h, static_cast<uint64_t>(x->qty.raw()));
    h = mix(h, x->takerAccount);
    h = mix(h, x->makerAccount);
  }
  else if (const auto* x = std::get_if<MmpTriggered>(&e))
  {
    h = mix(h, 10);
    h = mix(h, x->accountId);
    h = mix(h, x->symbol);
  }
  else if (const auto* x = std::get_if<FeeCharged>(&e))
  {
    h = mix(h, 11);
    h = mix(h, x->id);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->fee.raw()));
    h = mix(h, x->maker ? 1U : 0U);
    h = mix(h, x->account);
  }
  else if (const auto* x = std::get_if<Liquidation>(&e))
  {
    h = mix(h, 12);
    h = mix(h, x->account);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->qty.raw()));
    h = mix(h, static_cast<uint64_t>(x->price.raw()));
    h = mix(h, x->bankrupt ? 1U : 0U);
    h = mix(h, x->adl ? 1U : 0U);
  }
  else if (const auto* x = std::get_if<BalanceUpdate>(&e))
  {
    h = mix(h, 13);
    h = mix(h, x->account);
    h = mix(h, x->asset);
    h = mix(h, static_cast<uint64_t>(x->availableRaw));
    h = mix(h, static_cast<uint64_t>(x->reservedRaw));
    h = mix(h, static_cast<uint64_t>(x->reason));
  }
  else if (const auto* x = std::get_if<TradingStatusChanged>(&e))
  {
    h = mix(h, 14);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->status));
    h = mix(h, static_cast<uint64_t>(x->reason));
    h = mix(h, static_cast<uint64_t>(x->untilNs));  // sequencer-ts deadline: reproducible
  }
  else if (const auto* x = std::get_if<DerivativesUpdated>(&e))
  {
    h = mix(h, 15);
    h = mix(h, x->symbol);
    h = mix(h, static_cast<uint64_t>(x->mark.raw()));
    h = mix(h, static_cast<uint64_t>(x->fundingRateRaw));
    h = mix(h, static_cast<uint64_t>(x->nextFundingNs));
    h = mix(h, static_cast<uint64_t>(x->openInterest.raw()));
  }
  return h;
}

}  // namespace flox::venue
