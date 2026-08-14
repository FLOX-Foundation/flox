/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The old ResendBuffer (a per-session OutboundEvent log) was removed: the
 * per-account resend log now lives in SessionRegistry (session_registry.h),
 * which stores the SERIALIZED frames with their embedded seq -- replay is a
 * byte-for-byte repeat of the original reports, reachable from the wire via
 * the SBE ResendRequest verb. Keeping an unreachable event-level duplicate
 * here would just be dead forest.
 *
 * GapDetector is the CLIENT-side market-data sequencer. It consumes the raw
 * (possibly reordered, duplicated, gapped) datagram stream and delivers
 * messages strictly in seq order per (symbol, epoch):
 *
 * - Keyed by symbol: multiple per-symbol publishers may share one multicast
 *   group; every root block carries the symbol, so the detector demultiplexes
 *   before any gap accounting.
 * - Reordering: a message ahead of the next expected seq is HELD, not
 *   delivered; when the missing seq arrives (a late, reordered datagram --
 *   NOT a duplicate) the contiguous run drains in order. Nothing held is ever
 *   lost: if held messages exceed maxHeld the gap is abandoned and the held
 *   run is delivered in seq order (the consumer was already told about the
 *   gap and can re-snapshot).
 * - Gap reporting: onGap fires once when a gap opens (fromSeq = first missing
 *   seq) and RE-FIRES every reraiseAfter further received messages while the
 *   gap stays unfilled -- a lost resend does not strand the consumer.
 * - Epoch: a changed epoch means the publisher restarted; held state is
 *   dropped, the stream resets to seq 1 and onEpoch fires so the consumer can
 *   re-snapshot (its book is stale, not just gapped).
 * - reset() fast-forwards a stream after a snapshot was applied (next =
 *   snapshot lastSeq+1); held messages beyond the snapshot drain immediately.
 */
#pragma once

#include "flox-venue/market_data.h"

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

namespace flox::venue
{

class GapDetector
{
 public:
  struct Config
  {
    uint64_t reraiseAfter{16};  // re-report an unfilled gap after this many further messages
    size_t maxHeld{1024};       // held messages per symbol before the gap is abandoned
  };

  using DeliverFn = std::function<void(const MdMessage&)>;
  using GapFn = std::function<void(SymbolId, uint64_t epoch, uint64_t fromSeq)>;
  using EpochFn = std::function<void(SymbolId, uint64_t oldEpoch, uint64_t newEpoch)>;

  GapDetector() = default;
  explicit GapDetector(Config cfg) : cfg_(cfg) {}

  // Feed one received message. In-order messages (including a drained held
  // run) go to `deliver`; a new or re-raised gap goes to `onGap`; a publisher
  // restart goes to `onEpoch`. Duplicates are dropped silently.
  void observe(const MdMessage& m, const DeliverFn& deliver, const GapFn& onGap = {},
               const EpochFn& onEpoch = {})
  {
    Stream& s = streams_[m.symbol];
    if (!s.epochSet)
    {
      s.epoch = m.epoch;
      s.epochSet = true;
    }
    else if (m.epoch != s.epoch)
    {
      const uint64_t old = s.epoch;
      s = Stream{};
      s.epoch = m.epoch;
      s.epochSet = true;
      if (onEpoch)
      {
        onEpoch(m.symbol, old, m.epoch);
      }
    }

    bool freshGap = false;
    if (m.seq == s.next)
    {
      deliver(m);
      ++s.next;
      drainHeld(s, deliver);
    }
    else if (m.seq > s.next)
    {
      const bool held = s.held.emplace(m.seq, m).second;  // false: duplicate of a held one
      if (held && !s.gapOpen)
      {
        s.gapOpen = true;
        s.sinceReport = 0;
        freshGap = true;
        if (onGap)
        {
          onGap(m.symbol, s.epoch, s.next);
        }
      }
      if (s.held.size() > cfg_.maxHeld)
      {
        abandonGap(s, deliver);
      }
    }
    // else m.seq < s.next: duplicate below the delivery watermark -- drop

    s.gapOpen = !s.held.empty();
    if (s.gapOpen && !freshGap && ++s.sinceReport >= cfg_.reraiseAfter)
    {
      s.sinceReport = 0;
      if (onGap)
      {
        onGap(m.symbol, s.epoch, s.next);
      }
    }
  }

  // Fast-forward after a snapshot: the snapshot covered everything up to
  // nextSeq-1, so held messages below it are stale (pruned) and the contiguous
  // run at/after it drains through `deliver` now.
  void reset(SymbolId symbol, uint64_t epoch, uint64_t nextSeq, const DeliverFn& deliver = {})
  {
    Stream& s = streams_[symbol];
    s.epoch = epoch;
    s.epochSet = true;
    s.next = nextSeq;
    s.held.erase(s.held.begin(), s.held.lower_bound(nextSeq));
    if (deliver)
    {
      drainHeld(s, deliver);
    }
    s.gapOpen = !s.held.empty();
    s.sinceReport = 0;
  }

  // Next seq the symbol's stream will deliver (1 for an unseen symbol).
  uint64_t expected(SymbolId symbol) const
  {
    const auto it = streams_.find(symbol);
    return it == streams_.end() ? 1 : it->second.next;
  }

 private:
  struct Stream
  {
    uint64_t epoch{0};
    bool epochSet{false};
    uint64_t next{1};                    // next seq to deliver
    std::map<uint64_t, MdMessage> held;  // out-of-order messages awaiting the gap fill
    bool gapOpen{false};
    uint64_t sinceReport{0};
  };

  static void drainHeld(Stream& s, const DeliverFn& deliver)
  {
    while (!s.held.empty() && s.held.begin()->first == s.next)
    {
      deliver(s.held.begin()->second);
      s.held.erase(s.held.begin());
      ++s.next;
    }
  }

  // Held overflow: give up waiting for the resend and deliver everything held
  // in seq order (with the hole). The gap was already reported; a consumer
  // that needs a hole-free book re-snapshots.
  static void abandonGap(Stream& s, const DeliverFn& deliver)
  {
    while (!s.held.empty())
    {
      s.next = s.held.begin()->first;
      drainHeld(s, deliver);
    }
  }

  Config cfg_;
  std::unordered_map<SymbolId, Stream> streams_;
};

}  // namespace flox::venue
