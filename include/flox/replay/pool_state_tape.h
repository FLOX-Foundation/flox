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
#include "flox/backtest/cryptoswap_curve.h"
#include "flox/backtest/meteora_dlmm_curve.h"
#include "flox/backtest/ntoken_curve.h"
#include "flox/backtest/orca_whirlpool_curve.h"
#include "flox/backtest/raydium_clmm_curve.h"
#include "flox/backtest/raydium_cp_curve.h"
#include "flox/backtest/stableswap_curve.h"
#include "flox/backtest/weighted_curve.h"
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
// Covers every exact-curve venue; a Checkpoint is venue-shaped: reserves for CP,
// sqrt price + liquidity + ticks for a CLMM, the full balance vector for an n-token
// pool (StableSwap, Weighted; Cryptoswap also re-anchors its price scale), and the
// bin book + volatility state for Meteora DLMM. An n-token SwapDelta names the
// (i, j) pair explicitly; a swap off the connector's pair still moves the pool. A
// dedicated LiquidityDelta record (avoid re-anchoring on a mint/burn) is the
// remaining W23-T001 piece.

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
  StableSwap = 6,
  Weighted = 7,
  Cryptoswap = 8,
  MeteoraDlmm = 9,
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

// The n-token balances-shaped venues: a Checkpoint carries the full balance vector
// (Cryptoswap also carries its price scale), and a SwapDelta may name any (i, j)
// pair of the pool, not just the connector's.
inline bool isBalancesShaped(PoolVenue v)
{
  return v == PoolVenue::StableSwap || v == PoolVenue::Weighted || v == PoolVenue::Cryptoswap;
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
inline void putI64(std::vector<uint8_t>& b, int64_t v)
{
  putU64(b, static_cast<uint64_t>(v));
}
inline int64_t getI64(const uint8_t*& p)
{
  return static_cast<int64_t>(getU64(p));
}
inline void putU256Vec(std::vector<uint8_t>& b, const std::vector<u256>& vs)
{
  for (const u256& v : vs)
  {
    putU256(b, v);
  }
}
inline std::vector<u256> getU256Vec(const uint8_t*& p, std::size_t n)
{
  std::vector<u256> vs;
  vs.reserve(n);
  for (std::size_t k = 0; k < n; ++k)
  {
    vs.push_back(getU256(p));
  }
  return vs;
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

  // A Curve StableSwap venue: per-coin RATES, the contract A() value, and the raw
  // fee over 1e10. Uses checkpointBalances().
  void descriptorStableSwap(const std::vector<u256>& rates, uint64_t A, const u256& fee,
                            uint8_t baseDec, uint8_t quoteDec)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU8(p, static_cast<uint8_t>(PoolVenue::StableSwap));
    putU8(p, baseDec);
    putU8(p, quoteDec);
    putU64(p, rates.size());
    putU256Vec(p, rates);
    putU64(p, A);
    putU256(p, fee);
    frame(PoolRecord::Descriptor, p);
  }

  // A Balancer weighted venue: per-coin scaling factors and normalized weights
  // (1e18), and the swap fee (1e18). Uses checkpointBalances().
  void descriptorWeighted(const std::vector<u256>& scalingFactors,
                          const std::vector<u256>& weights, const u256& swapFee, uint8_t baseDec,
                          uint8_t quoteDec)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU8(p, static_cast<uint8_t>(PoolVenue::Weighted));
    putU8(p, baseDec);
    putU8(p, quoteDec);
    putU64(p, weights.size());
    putU256Vec(p, scalingFactors);
    putU256Vec(p, weights);
    putU256(p, swapFee);
    frame(PoolRecord::Descriptor, p);
  }

  // A Curve V2 cryptoswap venue: per-coin PRECISIONS and the contract A() / gamma()
  // and dynamic-fee parameters. The price scale is state, not a parameter -- the
  // chain repegs it -- so it rides in checkpointCryptoswap() and re-anchors there.
  void descriptorCryptoswap(const std::vector<u256>& precisions, uint64_t A, const u256& gamma,
                            const u256& midFee, const u256& outFee, const u256& feeGamma,
                            uint8_t baseDec, uint8_t quoteDec)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU8(p, static_cast<uint8_t>(PoolVenue::Cryptoswap));
    putU8(p, baseDec);
    putU8(p, quoteDec);
    putU64(p, precisions.size());
    putU256Vec(p, precisions);
    putU64(p, A);
    putU256(p, gamma);
    putU256(p, midFee);
    putU256(p, outFee);
    putU256(p, feeGamma);
    frame(PoolRecord::Descriptor, p);
  }

  // A Meteora DLMM venue: the bin step and the adaptive-fee parameters from the
  // LbPair account. The bin book and volatility state ride in checkpointDlmm().
  void descriptorDlmm(uint16_t binStep, const DlmmFeeParams& params, uint8_t baseDec,
                      uint8_t quoteDec)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU8(p, static_cast<uint8_t>(PoolVenue::MeteoraDlmm));
    putU8(p, baseDec);
    putU8(p, quoteDec);
    putU64(p, binStep);
    putU64(p, params.baseFactor);
    putU8(p, params.baseFeePowerFactor);
    putU64(p, params.variableFeeControl);
    putU64(p, params.filterPeriod);
    putU64(p, params.decayPeriod);
    putU64(p, params.reductionFactor);
    putU64(p, params.maxVolatilityAccumulator);
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

  // The full balance vector, for an n-token venue (StableSwap, Weighted).
  void checkpointBalances(int64_t tsNs, const std::vector<u256>& balances)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putU64(p, balances.size());
    putU256Vec(p, balances);
    frame(PoolRecord::Checkpoint, p);
  }

  // Cryptoswap state: the balances plus the price scale (n-1 entries, the price of
  // each coin k>=1 in coin 0). A repeg between checkpoints is re-anchored here.
  void checkpointCryptoswap(int64_t tsNs, const std::vector<u256>& balances,
                            const std::vector<u256>& priceScale)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putU64(p, balances.size());
    putU256Vec(p, balances);
    putU256Vec(p, priceScale);
    frame(PoolRecord::Checkpoint, p);
  }

  // Meteora DLMM state: the active bin, the swap-time clock, the volatility
  // accumulator state, and the populated bin book.
  void checkpointDlmm(int64_t tsNs, int32_t activeId, uint64_t timestamp,
                      const DlmmVolatility& vol, const std::vector<DlmmBin>& bins)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putI64(p, activeId);
    putU64(p, timestamp);
    putU64(p, vol.volatilityAccumulator);
    putU64(p, vol.volatilityReference);
    putI64(p, vol.indexReference);
    putU64(p, vol.lastUpdateTimestamp);
    putU64(p, bins.size());
    for (const DlmmBin& b : bins)
    {
      putI64(p, b.id);
      putU256(p, b.amountX);
      putU256(p, b.amountY);
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

  // An n-token swap names the pool's (i, j) pair explicitly, so a swap on a pair
  // the connector does not present still moves the shared state. Distinguished from
  // the directional record by payload size.
  void swapN(int64_t tsNs, uint8_t i, uint8_t j, const u256& amountIn)
  {
    std::vector<uint8_t> p;
    using namespace pool_tape_detail;
    putU64(p, static_cast<uint64_t>(tsNs));
    putU8(p, i);
    putU8(p, j);
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
        readSwap(rec, len);
        break;
      default:
        break;  // unknown record type: skipped via len
    }
  }

 private:
  void readDescriptor(const uint8_t* rec)
  {
    using namespace pool_tape_detail;
    _venue = static_cast<PoolVenue>(*rec++);
    _baseDec = *rec++;
    _quoteDec = *rec++;
    if (_venue == PoolVenue::StableSwap)
    {
      const uint64_t n = getU64(rec);
      _rates = getU256Vec(rec, n);
      _amp = getU64(rec);
      _feeWide = getU256(rec);
      return;
    }
    if (_venue == PoolVenue::Weighted)
    {
      const uint64_t n = getU64(rec);
      _scalingFactors = getU256Vec(rec, n);
      _weights = getU256Vec(rec, n);
      _feeWide = getU256(rec);
      return;
    }
    if (_venue == PoolVenue::Cryptoswap)
    {
      const uint64_t n = getU64(rec);
      _precisions = getU256Vec(rec, n);
      _amp = getU64(rec);
      _gamma = getU256(rec);
      _midFee = getU256(rec);
      _outFee = getU256(rec);
      _feeGamma = getU256(rec);
      return;
    }
    if (_venue == PoolVenue::MeteoraDlmm)
    {
      _binStep = static_cast<uint16_t>(getU64(rec));
      _dlmmParams.baseFactor = static_cast<uint32_t>(getU64(rec));
      _dlmmParams.baseFeePowerFactor = *rec++;
      _dlmmParams.variableFeeControl = static_cast<uint32_t>(getU64(rec));
      _dlmmParams.filterPeriod = static_cast<uint16_t>(getU64(rec));
      _dlmmParams.decayPeriod = static_cast<uint16_t>(getU64(rec));
      _dlmmParams.reductionFactor = static_cast<uint16_t>(getU64(rec));
      _dlmmParams.maxVolatilityAccumulator = static_cast<uint32_t>(getU64(rec));
      return;
    }
    _fee = getU64(rec);  // feeNum (CP) / feePips (CLMM) / tradeFeeRate (RaydiumCp)
    if (_venue == PoolVenue::ConstantProduct)
    {
      _feeDen = getU64(rec);
    }
    else if (_venue == PoolVenue::RaydiumCp)
    {
      _creatorFee = getU64(rec);
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
    else if (isBalancesShaped(_venue))
    {
      const uint64_t n = getU64(rec);
      std::vector<u256> balances = getU256Vec(rec, n);
      if (_venue == PoolVenue::StableSwap)
      {
        next = std::make_unique<StableSwapCurve>(std::move(balances), _rates, _amp, _feeWide);
      }
      else if (_venue == PoolVenue::Weighted)
      {
        next = std::make_unique<WeightedCurve>(std::move(balances), _scalingFactors, _weights,
                                               _feeWide);
      }
      else
      {
        std::vector<u256> scale = getU256Vec(rec, n - 1);
        next = std::make_unique<CryptoswapCurve>(std::move(balances), _precisions,
                                                 std::move(scale), _amp, _gamma, _midFee, _outFee,
                                                 _feeGamma);
      }
    }
    else if (_venue == PoolVenue::MeteoraDlmm)
    {
      const auto activeId = static_cast<int32_t>(getI64(rec));
      const uint64_t timestamp = getU64(rec);
      DlmmVolatility vol;
      vol.volatilityAccumulator = static_cast<uint32_t>(getU64(rec));
      vol.volatilityReference = static_cast<uint32_t>(getU64(rec));
      vol.indexReference = static_cast<int32_t>(getI64(rec));
      vol.lastUpdateTimestamp = getU64(rec);
      const uint64_t n = getU64(rec);
      std::vector<DlmmBin> bins;
      bins.reserve(n);
      for (uint64_t k = 0; k < n; ++k)
      {
        DlmmBin b;
        b.id = static_cast<int32_t>(getI64(rec));
        b.amountX = getU256(rec);
        b.amountY = getU256(rec);
        bins.push_back(b);
      }
      next = std::make_unique<MeteoraDlmmCurve>(activeId, _binStep, std::move(bins), _dlmmParams,
                                                vol, timestamp);
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
      bool same = old.size() == fresh.size();
      for (std::size_t k = 0; same && k < old.size(); ++k)
      {
        same = old[k] == fresh[k];
      }
      if (!same)
      {
        ++_drift;
      }
    }
    _curve = std::move(next);
    _conn.setCurve(*_curve);
    _conn.republish(tsNs);
  }

  // A directional record (41 bytes) swaps the connector's pair; an n-token record
  // (42 bytes) names (i, j) -- on the connector's pair it emits the trade, off it
  // the state still moves and the book republishes, but no trade prints on this
  // symbol (the swap happened on a pair this connector does not present).
  void readSwap(const uint8_t* rec, std::size_t len)
  {
    using namespace pool_tape_detail;
    const int64_t tsNs = static_cast<int64_t>(getU64(rec));
    if (!_curve)
    {
      return;
    }
    if (len == kSwapNPayload)
    {
      const std::size_t i = *rec++;
      const std::size_t j = *rec++;
      const u256 amountIn = getU256(rec);
      if (i == _conn.baseIndex() && j == _conn.quoteIndex())
      {
        _conn.onSwap(amountIn, true, tsNs);
      }
      else if (i == _conn.quoteIndex() && j == _conn.baseIndex())
      {
        _conn.onSwap(amountIn, false, tsNs);
      }
      else
      {
        _curve->applySwap(i, j, amountIn);
        _conn.republish(tsNs);
      }
      return;
    }
    const bool baseForQuote = *rec++ != 0;
    const u256 amountIn = getU256(rec);
    _conn.onSwap(amountIn, baseForQuote, tsNs);
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

  // [ts:u64][i:u8][j:u8][amountIn:u256] -- the n-token SwapDelta payload size,
  // one byte longer than the directional [ts][dir][amountIn].
  static constexpr std::size_t kSwapNPayload = 8 + 1 + 1 + 32;

  AmmDexConnector& _conn;
  std::unique_ptr<INTokenCurve> _curve;
  PoolVenue _venue{PoolVenue::ConstantProduct};
  uint8_t _baseDec{0};
  uint8_t _quoteDec{0};
  uint64_t _fee{0};
  uint64_t _feeDen{0};
  uint64_t _creatorFee{0};
  bool _creatorOnInput{true};
  uint64_t _amp{0};
  u256 _feeWide{0};
  std::vector<u256> _rates;
  std::vector<u256> _scalingFactors;
  std::vector<u256> _weights;
  std::vector<u256> _precisions;
  u256 _gamma{0};
  u256 _midFee{0};
  u256 _outFee{0};
  u256 _feeGamma{0};
  uint16_t _binStep{0};
  DlmmFeeParams _dlmmParams{};
  std::size_t _drift{0};
};

}  // namespace flox
