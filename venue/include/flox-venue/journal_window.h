/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * "What happened to account N between t1 and t2."
 *
 * The journal already replays end to end, and the recovery path proves it. The
 * question an operator actually arrives with is narrower and has a deadline: a
 * counterparty disputes a fill, and the answer has to be the venue's own
 * history rather than a reconstruction from reports the counterparty also
 * holds.
 *
 * This replays a snapshot plus its segments into a fresh engine and hands back
 * the events inside a window, with the sequencer timestamps they were produced
 * under. Two properties make the answer usable as evidence:
 *
 *  - the digest covers EVERY event replayed, not only the ones in the window.
 *    It is the same fold the engine's own determinism tests use, so it can be
 *    compared against a live run's digest: equal digests mean this replay took
 *    the same path the venue did, which is what makes the windowed extract
 *    worth anything. A digest over the window alone would agree with a run
 *    that diverged before the window opened and converged again inside it.
 *
 *  - a foreign format version throws, by name, from the load underneath. Not
 *    caught here: an operator investigating a dispute must not be handed a
 *    short history that looks complete. That is the same refusal recovery
 *    makes, for the same reason.
 *
 * Replay is engine-side: no sockets, no gateway, nothing from the perimeter.
 */
#pragma once

#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flox::venue
{

struct WindowEvent
{
  int64_t ts{0};  // sequencer time the event was produced under
  OutboundEvent event;
};

struct WindowResult
{
  std::vector<WindowEvent> events;  // inside the window, after the account filter
  // Over every event the replay produced, in order -- comparable with a live
  // run's digest. See the header comment for why it is not the window's own.
  uint64_t streamDigest{0};
  uint64_t recordsReplayed{0};
  uint64_t eventsTotal{0};     // before the window and account filters
  uint64_t eventsInWindow{0};  // after the window, before the account filter
};

struct WindowQuery
{
  int64_t fromNs{0};
  int64_t toNs{0};
  // 0 = every account. An event that names no account (market data, trades
  // between two other parties) is never attributed to a filtered account.
  uint64_t account{0};
};

// The account an event belongs to, or 0 when it belongs to nobody in
// particular. Only the order-lifecycle reports carry one; a public trade
// print does not, and attributing it to one of its two sides would be a
// guess that reads as fact in a dispute.
inline uint64_t accountOf(const OutboundEvent& e) noexcept
{
  if (const auto* x = std::get_if<OrderAccepted>(&e))
  {
    return x->account;
  }
  if (const auto* x = std::get_if<OrderRejected>(&e))
  {
    return x->account;
  }
  if (const auto* x = std::get_if<OrderExecuted>(&e))
  {
    return x->account;
  }
  if (const auto* x = std::get_if<OrderCanceled>(&e))
  {
    return x->account;
  }
  if (const auto* x = std::get_if<OrderModified>(&e))
  {
    return x->account;
  }
  return 0;
}

// Replay `snapshot` (may be empty) then `segments`, in the order given, and
// collect what falls inside the query.
//
// Throws JournalFormatError from the load underneath when any file carries a
// format version this build does not read.
template <class Book = MatchingBook>
WindowResult replayWindow(const SymbolConfig& cfg, const std::string& snapshot,
                          const std::vector<std::string>& segments, const WindowQuery& q)
{
  WindowResult out;
  int64_t eventTs = 0;  // the timestamp of the command being applied

  MatchingEngine<Book> eng(cfg, [&](const OutboundEvent& e)
                           {
                             ++out.eventsTotal;
                             out.streamDigest = hashEvent(out.streamDigest, e);
                             if (eventTs < q.fromNs || eventTs > q.toNs)
                             {
                               return;
                             }
                             ++out.eventsInWindow;
                             if (q.account != 0 && accountOf(e) != q.account)
                             {
                               return;
                             }
                             out.events.push_back(WindowEvent{eventTs, e}); }, Book{}, cfg.matchPolicy);

  if (!snapshot.empty())
  {
    for (const auto& [ts, cmd] : Journal::loadTimed(snapshot))
    {
      eventTs = ts;
      eng.applySnapshotRecord(cmd, ts);
      ++out.recordsReplayed;
    }
  }
  for (const std::string& seg : segments)
  {
    for (const auto& [ts, cmd] : Journal::loadTimed(seg))
    {
      eventTs = ts;
      eng.submit(cmd, ts);
      ++out.recordsReplayed;
    }
  }
  return out;
}

}  // namespace flox::venue
