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

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace flox::venue::engine
{

// The two things that are not supposed to happen, counted and said out loud.
//
// Both are diagnostics rather than state: neither is hashed, neither is
// written into a snapshot and neither is carried into a snapshot clone -- a
// clone is a fresh engine and starts its own tally. They are grouped because
// they answer the same kind of question ("did the venue have to refuse
// something it should never have been asked?") and because a counter with no
// report is a number nobody ever sees at the moment it moves.
//
// Nothing here is on a hot path: both are reached only when the venue is
// already in a situation it does not expect.
class Integrity
{
 public:
  // A snapshot-only record arrived through live traffic. Dropped rather than
  // rejected per-order, which is deterministic on replay too: a journaled
  // stray record is dropped identically. `tag` is the command's alternative
  // index, the only thing about it worth printing.
  void dropSnapshotRecord(std::size_t tag)
  {
    ++dropped_;
    std::fprintf(stderr, "flox-venue: dropped snapshot-only record (tag %zu) from live traffic\n",
                 tag);
  }

  // A trade printed but could not be settled without creating value, so
  // nothing moved. Counted and logged rather than event-carried: adding an
  // event here would change the outbound stream on a path that must stay
  // unreachable.
  void reportUnsettled(uint64_t tradeId, SymbolId symbol, const char* why, uint64_t account)
  {
    ++unsettled_;
    std::fprintf(stderr,
                 "flox-venue: trade %llu on symbol %u NOT settled (%s, account %llu) -- "
                 "no value moved\n",
                 static_cast<unsigned long long>(tradeId), static_cast<unsigned>(symbol), why,
                 static_cast<unsigned long long>(account));
  }

  // Must stay at zero: a non-zero value means a fill reached clearing with
  // neither a reservation nor the balance to pay for it, and the venue
  // refused to invent the difference.
  uint64_t unsettledTrades() const noexcept { return unsettled_; }

  uint64_t droppedSnapshotRecords() const noexcept { return dropped_; }

 private:
  uint64_t unsettled_{0};
  uint64_t dropped_{0};
};

}  // namespace flox::venue::engine
