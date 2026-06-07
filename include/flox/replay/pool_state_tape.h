/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/backtest/constant_product_curve.h"
#include "flox/backtest/ntoken_curve.h"
#include "flox/backtest/orca_whirlpool_curve.h"
#include "flox/backtest/raydium_clmm_curve.h"
#include "flox/backtest/raydium_cp_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/util/int/i256.h"
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
// unmodelled mutation, including a liquidity change between checkpoints): before a
// Checkpoint re-anchors, the replayed state is compared to it. All amounts are u256,
// 32 bytes big-endian, chain-native, no scaling.
//
// Covers the constant-product and concentrated-liquidity venues; a Checkpoint is
// venue-shaped (reserves for CP, sqrt price + liquidity + ticks for a CLMM). A
// dedicated LiquidityDelta record (avoid re-anchoring on a mint/burn) and the binary
// segment container / ReplayEvent integration are the remaining W23-T001 pieces.

enum class PoolRecord : uint8_t
{
  Descriptor = 1,
  Checkpoint = 2,
  SwapDelta = 3,
};

enum class PoolVenue : uint8_t
{
  ConstantProduct = 1,
  UniswapV3 = 2,
  OrcaWhirlpool = 3,
  RaydiumClmm = 4,
  RaydiumCp = 5,
};

inline bool isClmm(PoolVenue v)
{
  return v == PoolVenue::UniswapV3 || v == PoolVenue::OrcaWhirlpool || v == PoolVenue::RaydiumClmm;
}

// The two-reserve (x*y=k style) venues: a Checkpoint carries the two reserves, and a
// SwapDelta moves them through the venue's exact curve.
inline bool isTwoReserve(PoolVenue v)
{
  return v == PoolVenue::ConstantProduct || v == PoolVenue::RaydiumCp;
}

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
inline void putI256(std::vector<uint8_t>& b, const i256& v)
{
  putU8(b, v.neg ? 1 : 0);
  putU256(b, v.mag);
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
inline i256 getI256(const uint8_t*& p)
{
  const bool neg = *p++ != 0;
  return i256(getU256(p), neg);
}
}  // namespace pool_tape_detail

// Iterate a framed pool-state tape, calling fn(type, payload, len) per record. The
// frame is [type:u8][len:u64][payload]; the payload is handed back as a pointer so a
// caller can replay it (PoolStateReplay) or transcode it onto another container (the
// binary-log timeline) without copying.
template <typename F>
inline void forEachPoolRecord(const std::vector<uint8_t>& tape, F&& fn)
{
  const uint8_t* p = tape.data();
  const uint8_t* end = p + tape.size();
  while (p < end)
  {
    const auto type = static_cast<PoolRecord>(*p++);
    const uint64_t len = pool_tape_detail::getU64(p);
    const uint8_t* rec = p;
    p += len;
    fn(type, rec, len);
  }
}

// Writes pool-state records into a byte buffer. Each record is framed
// [type:u8][len:u64][payload], so a reader that does not know a type can skip it.
class PoolStateWriter
{
 public:
  explicit PoolStateWriter(std::vector<uint8_t>& out) : _out(out) {}

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

  // A Raydium constant-product (CPMM) venue: rates over a 1e6 denominator, plus the
  // creator-fee mode. Uses the two-reserve checkpoint().
  void descriptorRaydiumCp(uint64_t tradeFeeRate, uint64_t creatorFeeRate, bool creatorFeeOnInput,
                           uint8_t baseDec, uint8_t quoteDec)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU8(p, static_cast<uint8_t>(PoolVenue::RaydiumCp));
    putU8(p, baseDec);
    putU8(p, quoteDec);
    putU64(p, tradeFeeRate);
    putU64(p, creatorFeeRate);
    putU8(p, creatorFeeOnInput ? 1 : 0);
    frame(PoolRecord::Descriptor, p);
  }

  // A concentrated-liquidity venue: the fee in hundredths of a basis point.
  void descriptorClmm(PoolVenue venue, uint32_t feePips, uint8_t baseDec, uint8_t quoteDec)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU8(p, static_cast<uint8_t>(venue));
    putU8(p, baseDec);
    putU8(p, quoteDec);
    putU64(p, feePips);
    frame(PoolRecord::Descriptor, p);
  }

  void checkpoint(int64_t tsNs, const u256& reserve0, const u256& reserve1)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putU256(p, reserve0);
    putU256(p, reserve1);
    frame(PoolRecord::Checkpoint, p);
  }

  void checkpointClmm(int64_t tsNs, const u256& sqrtPrice, const u256& liquidity,
                      const std::vector<ClTick>& ticks)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putU256(p, sqrtPrice);
    putU256(p, liquidity);
    putU64(p, ticks.size());
    for (const ClTick& t : ticks)
    {
      putU256(p, t.sqrtRatio);
      putI256(p, t.liquidityNet);
    }
    frame(PoolRecord::Checkpoint, p);
  }

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
// exact curve and re-points the connector; a SwapDelta is applied via onSwap (moves
// the curve, emits the trade, republishes the book). Checkpoint-anchored: before a
// Checkpoint re-anchors, the replayed composition is compared to the checkpoint's
// (via balances(), which works for every venue), and a mismatch is counted as drift.
class PoolStateReplay
{
 public:
  explicit PoolStateReplay(AmmDexConnector& connector) : _conn(connector) {}

  std::size_t driftCount() const { return _drift; }
  const INTokenCurve* curve() const { return _curve.get(); }

  void run(const std::vector<uint8_t>& tape)
  {
    forEachPoolRecord(tape, [this](PoolRecord type, const uint8_t* rec, uint64_t len)
                      { step(type, rec, len); });
  }

  // Apply one record's payload (the bytes after the [type][len] frame). This is the
  // single entry point both run() and a binary-log-timeline driver use, so a
  // PoolState event read off the engine's replay stream replays identically to one
  // read off a standalone tape.
  void step(PoolRecord type, const uint8_t* rec, std::size_t len)
  {
    (void)len;
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

 private:
  void readDescriptor(const uint8_t* rec)
  {
    _venue = static_cast<PoolVenue>(*rec++);
    _baseDec = *rec++;
    _quoteDec = *rec++;
    _fee = pool_tape_detail::getU64(rec);  // feeNum (CP) / feePips (CLMM) / tradeFeeRate (RaydiumCp)
    if (_venue == PoolVenue::ConstantProduct)
    {
      _feeDen = pool_tape_detail::getU64(rec);
    }
    else if (_venue == PoolVenue::RaydiumCp)
    {
      _creatorFee = pool_tape_detail::getU64(rec);
      _creatorOnInput = *rec++ != 0;
    }
  }

  void readCheckpoint(const uint8_t* rec)
  {
    using namespace pool_tape_detail;
    const int64_t tsNs = static_cast<int64_t>(getU64(rec));
    std::unique_ptr<INTokenCurve> next;
    if (isTwoReserve(_venue))
    {
      const u256 r0 = getU256(rec);
      const u256 r1 = getU256(rec);
      if (_venue == PoolVenue::RaydiumCp)
      {
        next = std::make_unique<RaydiumCpCurve>(r0, r1, _fee, _creatorFee, _creatorOnInput);
      }
      else
      {
        next = std::make_unique<ConstantProductCurve>(r0, r1, _fee, _feeDen);
      }
    }
    else
    {
      const u256 sqrtP = getU256(rec);
      const u256 L = getU256(rec);
      const uint64_t n = getU64(rec);
      std::vector<ClTick> ticks;
      ticks.reserve(n);
      for (uint64_t k = 0; k < n; ++k)
      {
        const u256 s = getU256(rec);
        const i256 net = getI256(rec);
        ticks.push_back({s, net});
      }
      next = buildClmm(sqrtP, L, std::move(ticks));
    }

    // Drift check via the composition, which every venue exposes.
    if (_curve)
    {
      const std::vector<u256> old = _curve->balances();
      const std::vector<u256>& fresh = next->balances();
      if (old.size() != fresh.size() || !(old[0] == fresh[0]) || !(old[1] == fresh[1]))
      {
        ++_drift;
      }
    }
    _curve = std::move(next);
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

  std::unique_ptr<INTokenCurve> buildClmm(const u256& sqrtP, const u256& L,
                                          std::vector<ClTick> ticks) const
  {
    const auto fee = static_cast<uint32_t>(_fee);
    switch (_venue)
    {
      case PoolVenue::UniswapV3:
        return std::make_unique<ConcentratedLiquidityCurve>(sqrtP, L, fee, std::move(ticks));
      case PoolVenue::OrcaWhirlpool:
        return std::make_unique<OrcaWhirlpoolCurve>(sqrtP, L, fee, std::move(ticks));
      case PoolVenue::RaydiumClmm:
        return std::make_unique<RaydiumClmmCurve>(sqrtP, L, fee, std::move(ticks));
      default:
        throw std::runtime_error("pool-state tape: unknown CLMM venue");
    }
  }

  AmmDexConnector& _conn;
  std::unique_ptr<INTokenCurve> _curve;
  PoolVenue _venue{PoolVenue::ConstantProduct};
  uint8_t _baseDec{0};
  uint8_t _quoteDec{0};
  uint64_t _fee{0};
  uint64_t _feeDen{0};
  uint64_t _creatorFee{0};
  bool _creatorOnInput{true};
  std::size_t _drift{0};
};

}  // namespace flox
