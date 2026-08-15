/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Pluggable wire encoding for the unicast market-data distribution server
 * (md_distribution.h).
 *
 * Transport and encoding are independent axes. The distribution server owns
 * the transport and the semantics -- subscriptions, the atomic snapshot, the
 * bounded resend ring, seq/epoch, the slow-consumer policy -- and knows
 * nothing about the bytes. An MdEncoder owns the bytes and knows nothing about
 * the semantics: it turns the peer's inbound stream into MdRequest values and
 * turns snapshots / increments / rejects / heartbeats into wire bytes. Adding
 * an encoding is writing one MdEncoder and registering it; no distribution
 * logic changes.
 *
 * One encoder instance per connection, so an encoder may hold per-session
 * state (a sequence counter, the peer's request ids). Every call on it is
 * serialized by the session, on either the connection's reader thread or the
 * publishing thread -- an implementation needs no locking of its own.
 *
 * FRAMING BELONGS TO THE ENCODER. Every method appends COMPLETE, self-framed
 * wire bytes to `out` (length prefixes, FIX BodyLength/CheckSum, ... all
 * included); the session writes those bytes to the socket verbatim. That is
 * what lets one server carry a length-prefixed binary encoding and a
 * self-delimiting text encoding on the same port. Appending -- not clearing --
 * lets one call batch a whole snapshot into a single queued write.
 */
#pragma once

#include "flox-venue/market_data.h"
#include "flox-venue/sbe_md_codec.h"

#include "flox/util/transport.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace flox::venue
{

enum class MdRequestKind : uint8_t
{
  None,         // a session-level frame that needs no distribution action (heartbeat, logon)
  Subscribe,    // start (or resume) streaming a symbol on this connection
  Unsubscribe,  // stop streaming a symbol; other symbols keep flowing
  Resend,       // fill a gap: replay from `fromSeq`, or say a snapshot is needed
  Close,        // the peer asked for an orderly shutdown
};

// One decoded distribution request. A single inbound message may carry
// several (a market-data request naming several symbols decodes into one
// Subscribe per symbol), which is why parse() fills a vector.
struct MdRequest
{
  MdRequestKind kind{MdRequestKind::None};
  SymbolId symbol{};
  uint64_t fromSeq{0};       // Subscribe/Resend: 0 = full snapshot, >0 = resume at that seq
  bool snapshotOnly{false};  // serve one snapshot, then drop the subscription
};

enum class MdRejectReason : uint8_t
{
  UnknownSymbol,       // no publisher registered for the symbol
  UnsupportedRequest,  // the request type itself cannot be served
};

class MdEncoder
{
 public:
  enum class Parse : uint8_t
  {
    Need,  // an incomplete message at the head of the buffer: read more bytes
    Ok,    // `consumed` bytes decoded; `out` and/or `reply` filled (both may be empty)
    Bad,   // unparseable / protocol violation: the session is torn down
  };

  virtual ~MdEncoder() = default;

  // Human-readable encoding name, for logs and tests.
  virtual const char* name() const noexcept = 0;

  // Decode ONE message from the head of [in, in+n). On Ok, `consumed` is that
  // message's length in bytes, `out` gets the distribution requests it carries
  // and `reply` gets any session-level answer the encoding owes the peer (a
  // logon acknowledgement, a heartbeat echo, a protocol-level reject) -- the
  // session queues `reply` verbatim before acting on `out`.
  virtual Parse parse(const uint8_t* in, size_t n, size_t& consumed, std::vector<MdRequest>& out,
                      std::vector<uint8_t>& reply) = 0;

  // Full-book snapshot for `symbol`, consistent with the incremental stream at
  // snap.lastSeq under snap.epoch.
  virtual void snapshot(SymbolId symbol, const MdSnapshot& snap, std::vector<uint8_t>& out) = 0;

  // One incremental message. May append nothing when the encoding has no form
  // for it (the session then queues nothing).
  virtual void increment(const MdMessage& m, std::vector<uint8_t>& out) = 0;

  // "The increments you asked for are gone, a snapshot follows." Encodings
  // whose snapshot already carries that meaning append nothing.
  virtual void snapshotRequired(SymbolId symbol, uint64_t epoch, uint64_t lastSeq,
                                std::vector<uint8_t>& out) = 0;

  // A request that cannot be served at all; no subscription is left behind.
  virtual void reject(SymbolId symbol, MdRejectReason reason, std::vector<uint8_t>& out) = 0;

  // Liveness beat, sent when the connection has been quiet.
  virtual void heartbeat(std::vector<uint8_t>& out) = 0;
};

// SBE encoding: the same schema and the same templates the multicast feed and
// the recovery channel use (sbe_md_codec.h), carried over TCP in the same
// length-prefixed frames (flox::net framing). A consumer that already decodes
// the multicast feed decodes this stream with the same code -- the only new
// templates are the session verbs (Subscribe / Unsubscribe / SubscribeReject).
//
// Inbound verbs: Subscribe (11), Unsubscribe (12), ResendRequest (7). An empty
// frame is the client's heartbeat; the server answers idleness with the same.
class SbeMdEncoder final : public MdEncoder
{
 public:
  // Lead byte of an SBE distribution stream: the high byte of the u32
  // big-endian frame length. Distribution frames are tens of bytes, so it is
  // always zero -- which is what distinguishes them from a text encoding.
  static constexpr uint8_t kLeadByte = 0x00;

  const char* name() const noexcept override { return "SBE"; }

  Parse parse(const uint8_t* in, size_t n, size_t& consumed, std::vector<MdRequest>& out,
              std::vector<uint8_t>& reply) override
  {
    (void)reply;
    if (n < 4)
    {
      return Parse::Need;
    }
    const uint32_t len = (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
                         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
    if (len > net::kMaxFrame)
    {
      return Parse::Bad;  // hostile length prefix: never reserve on it
    }
    if (n < 4 + static_cast<size_t>(len))
    {
      return Parse::Need;
    }
    consumed = 4 + static_cast<size_t>(len);
    if (len == 0)
    {
      return Parse::Ok;  // client heartbeat
    }
    const uint8_t* p = in + 4;
    switch (static_cast<SbeMdCodec::Tmpl>(SbeMdCodec::templateId(p, len)))
    {
      case SbeMdCodec::Tmpl::Subscribe:
      {
        MdSubscribeRequest s{};
        if (!SbeMdCodec::decode(p, len, s))
        {
          return Parse::Bad;
        }
        out.push_back(MdRequest{MdRequestKind::Subscribe, s.symbol, s.fromSeq, s.snapshotOnly});
        return Parse::Ok;
      }
      case SbeMdCodec::Tmpl::Unsubscribe:
      {
        MdUnsubscribeRequest u{};
        if (!SbeMdCodec::decode(p, len, u))
        {
          return Parse::Bad;
        }
        out.push_back(MdRequest{MdRequestKind::Unsubscribe, u.symbol, 0, false});
        return Parse::Ok;
      }
      case SbeMdCodec::Tmpl::ResendRequest:
      {
        MdResendRequest r{};
        if (!SbeMdCodec::decode(p, len, r))
        {
          return Parse::Bad;
        }
        out.push_back(MdRequest{MdRequestKind::Resend, r.symbol, r.fromSeq, false});
        return Parse::Ok;
      }
      default:
        return Parse::Bad;  // not an inbound verb
    }
  }

  void snapshot(SymbolId symbol, const MdSnapshot& snap, std::vector<uint8_t>& out) override
  {
    appendFrame(out, MdSnapshotBegin{symbol, snap.epoch, snap.lastSeq,
                                     static_cast<uint32_t>(snap.orders.size())});
    // State before book: the subscriber knows whether the instrument is halted,
    // paused or auctioning -- and what the position is worth -- before it
    // applies a single order. orderCount stays the AddOrder count.
    if (snap.hasStatus)
    {
      appendFrame(out, snap.status);
    }
    if (snap.hasDerivatives)
    {
      appendFrame(out, snap.derivatives);
    }
    for (const MdMessage& m : snap.orders)
    {
      appendFrame(out, m);
    }
    appendFrame(out, MdSnapshotEnd{symbol, snap.epoch, snap.lastSeq});
  }

  void increment(const MdMessage& m, std::vector<uint8_t>& out) override { appendFrame(out, m); }

  void snapshotRequired(SymbolId symbol, uint64_t epoch, uint64_t lastSeq,
                        std::vector<uint8_t>& out) override
  {
    appendFrame(out, MdSnapshotRequired{symbol, epoch, lastSeq});
  }

  void reject(SymbolId symbol, MdRejectReason reason, std::vector<uint8_t>& out) override
  {
    appendFrame(out, MdSubscribeReject{symbol, reason == MdRejectReason::UnknownSymbol
                                                   ? MdSubscribeRejectReason::UnknownSymbol
                                                   : MdSubscribeRejectReason::UnsupportedRequest});
  }

  // An empty frame: a beat that costs four bytes and needs no template.
  void heartbeat(std::vector<uint8_t>& out) override
  {
    const uint8_t hdr[4] = {0, 0, 0, 0};
    out.insert(out.end(), hdr, hdr + 4);
  }

 private:
  template <typename T>
  void appendFrame(std::vector<uint8_t>& out, const T& msg)
  {
    SbeMdCodec::encode(msg, scratch_);
    const size_t n = scratch_.size();
    const uint8_t hdr[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                            static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
    out.insert(out.end(), hdr, hdr + 4);
    out.insert(out.end(), scratch_.begin(), scratch_.end());
  }

  std::vector<uint8_t> scratch_;
};

}  // namespace flox::venue
