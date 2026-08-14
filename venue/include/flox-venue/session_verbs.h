/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * SBE session-layer verbs, served at the gateway BEFORE the frame reaches the
 * order-entry decoder (they are session state, never matched or journaled):
 *
 *  - ResendRequest{fromSeq}: replay exec reports with seq >= fromSeq from the
 *    account's resend log (SessionRegistry). A fromSeq older than the retained
 *    log answers with SnapshotRequired{lastSeq} -- an explicit signal, never a
 *    silent hole.
 *  - AccountSnapshotRequest: reconnect reconciliation from
 *    MatchingEngine::snapshotAccount -- open orders as a series of Accepted
 *    frames (restingOnBook=0 for pending stops), terminated by
 *    SnapshotEnd{position, lastSeq}. Snapshot frames are session-layer:
 *    unsequenced (seq 0) and deliberately NOT in the resend log (a resend
 *    replays what was originally sequenced; replaying a point-in-time
 *    snapshot there would be stale and meaningless). SnapshotEnd carries the
 *    stream's lastSeq so the client resumes gap detection from the exact
 *    point (registry lastSeq is untouched by the snapshot itself).
 *  - SetSessionConfig{codEnabled}: wire negotiation of per-session
 *    cancel-on-disconnect. Fire-and-forget with an observable effect (a later
 *    disconnect sweeps or keeps the orders); no reply frame. Decoded by
 *    makeSbeSessionConfigVerb, applied by the gateway to the live session.
 */
#pragma once

#include "flox-venue/sbe_order_entry_codec.h"
#include "flox-venue/session_registry.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace flox::venue
{

// Build the SBE verb handler over a matching engine (any Book instantiation).
// The engine reference must outlive the gateway.
template <class Engine>
SessionVerbHandler makeSbeSessionVerbs(Engine& engine)
{
  return [&engine](const uint8_t* p, size_t n, uint64_t account, SessionRegistry& registry) -> bool
  {
    const uint16_t tid = SbeOrderEntryCodec::templateId(p, n);
    if (tid == static_cast<uint16_t>(SbeOrderEntryCodec::InTmpl::ResendRequest))
    {
      const auto fromSeq = SbeOrderEntryCodec::decodeResendRequest(p, n);
      if (!fromSeq)
      {
        return true;  // malformed verb: consumed, nothing to serve
      }
      if (registry.resendFrom(account, *fromSeq) == SessionRegistry::ResendResult::TooOld)
      {
        std::vector<uint8_t> rsp;
        SbeOrderEntryCodec::encodeSnapshotRequired(registry.lastSeq(account), rsp);
        registry.enqueueRaw(account, std::move(rsp));
      }
      return true;
    }
    if (tid == static_cast<uint16_t>(SbeOrderEntryCodec::InTmpl::AccountSnapshotRequest))
    {
      const auto snap = engine.snapshotAccount(account);
      const SymbolId sym = engine.config().id;
      std::vector<uint8_t> frame;
      uint32_t count = 0;
      for (const auto& o : snap.openOrders)
      {
        SbeOrderEntryCodec::encode(
            OutboundEvent{OrderAccepted{o.id, sym, o.side, o.price, o.leaves, true, Quantity{},
                                        account}},
            frame);
        registry.enqueueRaw(account, frame);
        ++count;
      }
      for (const auto& st : snap.pendingStops)
      {
        SbeOrderEntryCodec::encode(
            OutboundEvent{OrderAccepted{st.id, sym, st.side, st.trigger, st.quantity, false,
                                        Quantity{}, account}},
            frame);
        registry.enqueueRaw(account, frame);
        ++count;
      }
      SbeOrderEntryCodec::SnapshotEnd se;
      se.account = account;
      se.positionQtyRaw = snap.positionQty;
      se.positionEntryRaw = snap.positionEntry.raw();
      se.openOrders = count;
      se.lastSeq = registry.lastSeq(account);
      SbeOrderEntryCodec::encodeSnapshotEnd(se, frame);
      registry.enqueueRaw(account, std::move(frame));
      return true;
    }
    return false;
  };
}

// SBE decoder for the session-config verb (SetSessionConfig -> per-session
// cancel-on-disconnect). The gateway applies the returned update to the live
// session; there is no reply frame (fire-and-forget by design).
inline SessionConfigHandler makeSbeSessionConfigVerb()
{
  return [](const uint8_t* p, size_t n) -> std::optional<SessionConfigUpdate>
  {
    const auto cod = SbeOrderEntryCodec::decodeSetSessionConfig(p, n);
    if (!cod)
    {
      return std::nullopt;
    }
    return SessionConfigUpdate{*cod};
  };
}

}  // namespace flox::venue
