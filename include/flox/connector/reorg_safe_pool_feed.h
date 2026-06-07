/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/ntoken_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/util/int/u256.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace flox
{

// Drives a live pool through the same SwapDelta stream a recorded tape uses, but with
// the concerns a historical replay ignores: a chain can roll back (an EVM reorg, an
// unfinalised Solana slot), so a swap is optimistic until its block is finalised.
//
// The feed keeps a finalised curve (the last irreversible state) and a buffer of
// optimistic swaps applied on top. A reorg drops the optimistic swaps above the
// rolled-back height and rebuilds the working curve from the finalised one -- the
// curve state can never be corrupted by a block that did not stick. Finalising folds
// the now-irreversible swaps into the finalised curve, so they survive any later
// reorg. With no reorg, the working curve is exactly what the tape replay produces
// from the same deltas, so a strategy sees an identical book live and from a tape.
class ReorgSafePoolFeed
{
 public:
  // initial is the finalised state at the start (a checkpoint read from chain).
  ReorgSafePoolFeed(AmmDexConnector& connector, std::unique_ptr<INTokenCurve> initial)
      : _conn(connector), _finalized(std::move(initial)), _working(_finalized->clone())
  {
    _conn.setCurve(*_working);
  }

  // A swap observed at chain height (block number / slot). Applied to the working
  // curve immediately (the live book moves), and buffered until its height finalises.
  void onSwap(uint64_t height, bool baseForQuote, const u256& amountIn, int64_t tsNs = 0)
  {
    _pending.push_back({height, baseForQuote, amountIn});
    _lastHeight = height;
    _conn.onSwap(amountIn, baseForQuote, tsNs);
  }

  // Everything at or below height is irreversible: fold those swaps into the
  // finalised curve and drop them from the optimistic buffer. The working curve is
  // unchanged (it already has them).
  void finalize(uint64_t height)
  {
    std::vector<PendingSwap> stillPending;
    stillPending.reserve(_pending.size());
    for (const PendingSwap& s : _pending)
    {
      if (s.height <= height)
      {
        const std::size_t i = s.baseForQuote ? _conn.baseIndex() : _conn.quoteIndex();
        const std::size_t j = s.baseForQuote ? _conn.quoteIndex() : _conn.baseIndex();
        _finalized->applySwap(i, j, s.amountIn);
      }
      else
      {
        stillPending.push_back(s);
      }
    }
    _pending = std::move(stillPending);
    _finalizedHeight = height;
  }

  // Roll back to height: discard the optimistic swaps above it (the blocks that did
  // not stick) and rebuild the working curve from the finalised one. Finalised swaps
  // are not in the buffer, so they are never rolled back.
  void reorg(uint64_t height)
  {
    std::vector<PendingSwap> survivors;
    survivors.reserve(_pending.size());
    for (const PendingSwap& s : _pending)
    {
      if (s.height <= height)
      {
        survivors.push_back(s);
      }
    }
    _pending = std::move(survivors);
    rebuildWorking();
  }

  const INTokenCurve* curve() const { return _working.get(); }
  std::size_t pendingCount() const { return _pending.size(); }
  uint64_t finalizedHeight() const { return _finalizedHeight; }
  uint64_t lastHeight() const { return _lastHeight; }

 private:
  struct PendingSwap
  {
    uint64_t height;
    bool baseForQuote;
    u256 amountIn;
  };

  void rebuildWorking()
  {
    _working = _finalized->clone();
    _conn.setCurve(*_working);
    for (const PendingSwap& s : _pending)
    {
      _conn.onSwap(s.amountIn, s.baseForQuote, 0);
    }
  }

  AmmDexConnector& _conn;
  std::unique_ptr<INTokenCurve> _finalized;
  std::unique_ptr<INTokenCurve> _working;
  std::vector<PendingSwap> _pending;
  uint64_t _finalizedHeight{0};
  uint64_t _lastHeight{0};
};

}  // namespace flox
