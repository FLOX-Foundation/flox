/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/binary_format_v1.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace flox::replay
{

// Encodes a stream of L2 book snapshots into anchor snapshots plus
// deltas. The on-disk format already supports both event types
// (BookSnapshot + BookDelta in EventType); this class is the
// state-keeping layer that decides which one to emit and computes
// the level diff for deltas.
//
// Convention: in a delta payload, a level with qty_raw == 0 means
// "remove this price level"; a level with qty_raw > 0 means "set
// this price level to this quantity". A snapshot always contains
// the full sorted level list.
//
// Anchor cadence: every `anchor_every` events the encoder emits a
// full snapshot regardless of diff size. This keeps random-access
// reads cheap (a reader can seek to the most recent anchor and
// replay forward). 0 means "always anchor", which degenerates to
// the snapshot-only behaviour the existing writer has today.
class DeltaBookEncoder
{
 public:
  struct Output
  {
    bool is_delta{false};  // false = anchor snapshot
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
  };

  explicit DeltaBookEncoder(uint32_t anchor_every = 100);

  // Feed the encoder a full snapshot for one symbol. Returns either
  // a snapshot (when this is the first observation, an anchor due
  // by cadence, or the previous state was empty) or a delta of
  // changed levels. The returned vectors reference the encoder's
  // scratch buffers; copy out before the next encode() call on the
  // same symbol.
  Output encode(uint32_t symbol_id,
                const std::vector<BookLevel>& bids,
                const std::vector<BookLevel>& asks);

  // Reset the per-symbol state. Useful when reusing the encoder
  // across captures.
  void reset(uint32_t symbol_id);
  void resetAll();

 private:
  struct SymbolState
  {
    std::map<int64_t, int64_t> bid_levels;  // price -> qty
    std::map<int64_t, int64_t> ask_levels;
    uint32_t since_anchor{0};
  };

  static std::vector<BookLevel> diffSide(const std::map<int64_t, int64_t>& prev,
                                         const std::vector<BookLevel>& current);

  static void applySide(std::map<int64_t, int64_t>& state,
                        const std::vector<BookLevel>& levels);

  uint32_t _anchor_every;
  std::unordered_map<uint32_t, SymbolState> _state;
  Output _scratch;
};

// Reverse of the encoder. Maintains current book state per symbol;
// callers feed it events (snapshot or delta) and read full
// snapshots back. Useful when a downstream consumer expects
// snapshots but the tape stores deltas.
class DeltaBookReplayer
{
 public:
  struct Snapshot
  {
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
  };

  // Apply one event. type=0 (snapshot) replaces the level set;
  // type=1 (delta) merges in the level changes. Returns the
  // current full snapshot for the symbol.
  Snapshot apply(uint8_t type, uint32_t symbol_id,
                 const std::vector<BookLevel>& bids,
                 const std::vector<BookLevel>& asks);

  void reset(uint32_t symbol_id);
  void resetAll();

 private:
  struct SymbolState
  {
    std::map<int64_t, int64_t> bid_levels;
    std::map<int64_t, int64_t> ask_levels;
  };

  static std::vector<BookLevel> dumpSide(const std::map<int64_t, int64_t>& state, bool descending);

  std::unordered_map<uint32_t, SymbolState> _state;
};

}  // namespace flox::replay
