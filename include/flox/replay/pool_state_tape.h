/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/constant_product_curve.h"
#include "flox/backtest/ntoken_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/util/int/u256.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace flox
{

// A pool-state tape is a delta log: a Descriptor (the venue and its static
// parameters), periodic Checkpoints (the full state, ground truth), and the
// SwapDeltas that mutate it. A pool's state is derived by replaying the deltas
// through the exact curve -- a parsed swap replayed via applySwap reconstructs the
// post-state to the wei -- so the tape is compact and trades and state are one
// stream. Checkpoints anchor the replay and catch drift (an unobserved or
// unmodelled mutation): the worst case for any chain is a Checkpoint every event,
// which always works. All amounts are u256, 32 bytes big-endian, chain-native, no
// scaling.
//
// This is the chain-agnostic core (constant-product venue first); the per-chain
// ingest (EVM logs, Solana instructions) produces these records, and the binary
// segment container / ReplayEvent integration layer on top.

enum class PoolRecord : uint8_t
{
  Descriptor = 1,
  Checkpoint = 2,
  SwapDelta = 3,
};

enum class PoolVenue : uint8_t
{
  ConstantProduct = 1,
};

namespace pool_tape_detail
{
inline void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
inline void putU64(std::vector<uint8_t>& b, uint64_t v)
{
  for (int byte = 7; byte >= 0; --byte)
  {
    b.push_back(static_cast<uint8_t>(v >> (byte * 8)));
  }
}
inline void putU256(std::vector<uint8_t>& b, const u256& v)
{
  for (int limb = 3; limb >= 0; --limb)
  {
    for (int byte = 7; byte >= 0; --byte)
    {
      b.push_back(static_cast<uint8_t>(v.w[static_cast<std::size_t>(limb)] >> (byte * 8)));
    }
  }
}
inline uint64_t getU64(const uint8_t*& p)
{
  uint64_t v = 0;
  for (int byte = 0; byte < 8; ++byte)
  {
    v = (v << 8) | *p++;
  }
  return v;
}
inline u256 getU256(const uint8_t*& p)
{
  u256 v;
  for (int limb = 3; limb >= 0; --limb)
  {
    v.w[static_cast<std::size_t>(limb)] = getU64(p);
  }
  return v;
}
}  // namespace pool_tape_detail

// Writes pool-state records into a byte buffer. Each record is framed
// [type:u8][len:u64][payload], so a reader that does not know a type can skip it.
class PoolStateWriter
{
 public:
  explicit PoolStateWriter(std::vector<uint8_t>& out) : _out(out) {}

  // Constant-product venue: the fee numerator / denominator and the two decimals.
  void descriptorConstantProduct(uint64_t feeNum, uint64_t feeDen, uint8_t baseDec, uint8_t quoteDec)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU8(p, static_cast<uint8_t>(PoolVenue::ConstantProduct));
    putU8(p, baseDec);
    putU8(p, quoteDec);
    putU64(p, feeNum);
    putU64(p, feeDen);
    frame(PoolRecord::Descriptor, p);
  }

  // The full reserves at a point in time -- an anchor and a drift check.
  void checkpoint(int64_t tsNs, const u256& reserve0, const u256& reserve1)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putU256(p, reserve0);
    putU256(p, reserve1);
    frame(PoolRecord::Checkpoint, p);
  }

  // A swap: the trade, and the state mutation when replayed through the curve.
  void swap(int64_t tsNs, bool baseForQuote, const u256& amountIn)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putU8(p, baseForQuote ? 1 : 0);
    putU256(p, amountIn);
    frame(PoolRecord::SwapDelta, p);
  }

 private:
  void frame(PoolRecord type, const std::vector<uint8_t>& payload)
  {
    using namespace pool_tape_detail;
    putU8(_out, static_cast<uint8_t>(type));
    putU64(_out, payload.size());
    _out.insert(_out.end(), payload.begin(), payload.end());
  }

  std::vector<uint8_t>& _out;
};

// Replays a pool-state tape through an AmmDexConnector: a Checkpoint rebuilds the
// exact curve and re-points the connector at it; a SwapDelta is applied via the
// connector's onSwap (which moves the curve, emits the trade, and republishes the
// book). It is checkpoint-anchored: before a Checkpoint re-anchors, the replayed
// state is compared to it, and a mismatch is counted as drift (never silently
// carried). Returns the drift count.
class PoolStateReplay
{
 public:
  explicit PoolStateReplay(AmmDexConnector& connector) : _conn(connector) {}

  std::size_t driftCount() const { return _drift; }
  const INTokenCurve* curve() const { return _curve.get(); }

  void run(const std::vector<uint8_t>& tape)
  {
    const uint8_t* p = tape.data();
    const uint8_t* end = p + tape.size();
    while (p < end)
    {
      const auto type = static_cast<PoolRecord>(*p++);
      const uint64_t len = pool_tape_detail::getU64(p);
      const uint8_t* rec = p;
      p += len;
      switch (type)
      {
        case PoolRecord::Descriptor:
          readDescriptor(rec);
          break;
        case PoolRecord::Checkpoint:
          readCheckpoint(rec);
          break;
        case PoolRecord::SwapDelta:
          readSwap(rec);
          break;
        default:
          break;  // unknown record type: skipped via len
      }
    }
  }

 private:
  void readDescriptor(const uint8_t* rec)
  {
    _venue = static_cast<PoolVenue>(*rec++);
    _baseDec = *rec++;
    _quoteDec = *rec++;
    _feeNum = pool_tape_detail::getU64(rec);
    _feeDen = pool_tape_detail::getU64(rec);
  }

  void readCheckpoint(const uint8_t* rec)
  {
    const int64_t tsNs = static_cast<int64_t>(pool_tape_detail::getU64(rec));
    const u256 r0 = pool_tape_detail::getU256(rec);
    const u256 r1 = pool_tape_detail::getU256(rec);

    // Drift check: the replayed state must match this checkpoint; re-anchor either
    // way, but count a mismatch.
    if (_curve)
    {
      const std::vector<u256>& bal = _curve->balances();
      if (bal.size() < 2 || !(bal[0] == r0) || !(bal[1] == r1))
      {
        ++_drift;
      }
    }
    _curve = buildCurve(r0, r1);
    _conn.setCurve(*_curve);
    _conn.republish(tsNs);
  }

  void readSwap(const uint8_t* rec)
  {
    const int64_t tsNs = static_cast<int64_t>(pool_tape_detail::getU64(rec));
    const bool baseForQuote = *rec++ != 0;
    const u256 amountIn = pool_tape_detail::getU256(rec);
    if (_curve)
    {
      _conn.onSwap(amountIn, baseForQuote, tsNs);
    }
  }

  std::unique_ptr<INTokenCurve> buildCurve(const u256& r0, const u256& r1) const
  {
    if (_venue == PoolVenue::ConstantProduct)
    {
      return std::make_unique<ConstantProductCurve>(r0, r1, _feeNum, _feeDen);
    }
    throw std::runtime_error("pool-state tape: unknown venue");
  }

  AmmDexConnector& _conn;
  std::unique_ptr<INTokenCurve> _curve;
  PoolVenue _venue{PoolVenue::ConstantProduct};
  uint8_t _baseDec{0};
  uint8_t _quoteDec{0};
  uint64_t _feeNum{0};
  uint64_t _feeDen{0};
  std::size_t _drift{0};
};

}  // namespace flox
