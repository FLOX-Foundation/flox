/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX 4.4 session layer with recovery, on top of the FixCodec framing and the
 * SessionRegistry event log:
 *
 *  - Logon (35=A): HeartBtInt (108) adoption, ResetSeqNumFlag (141=Y) resets
 *    both sequence directions to 1 (the registry stream is cleared). Without
 *    141, the client's 34 is checked against the expected inbound seq: above
 *    -> our ResendRequest (35=2) after the Logon reply; below -> Logout with
 *    text and disconnect.
 *  - Liveness: outbound Heartbeat every HeartBtInt on the gateway's idle
 *    tick; no inbound traffic for 1.2 * HeartBtInt -> TestRequest (35=1); no
 *    answer for another 1.2 intervals -> disconnect (COD sweeps normally).
 *  - Their ResendRequest (35=2): application messages replay from the
 *    registry event log with PossDupFlag (43=Y) and OrigSendingTime (122);
 *    admin seq ranges collapse into SequenceReset-GapFill (35=4, 123=Y). A
 *    range older than the retained log is gap-filled up to the first
 *    available seq -- the client sees the trimmed part as an explicit hole;
 *    full state reconciliation is the SBE snapshot path. Served resends bump
 *    GatewayCounters::resendServed (same counter as the SBE path).
 *  - Our inbound gap: NO reorder buffer -- we send 35=2 and DROP every
 *    message above the hole (repeating the request at most once per
 *    HeartBtInt); the counterparty replays with PossDup, and a PossDup whose
 *    34 was already seen is silently dropped. Buffering out-of-order
 *    application traffic would let a gapped session run orders out of
 *    admission order; dropping is valid because the peer must resend anyway.
 *  - Unknown MsgType: an in-sequence message whose 35 is outside the known
 *    set (admin 0/1/2/4/5/A, application D/F/G) is answered with a session
 *    Reject (35=3: 45=RefSeqNum, 372=RefMsgType, 58=text) instead of being
 *    silently consumed. Application messages still flow through the normal
 *    decoder path -- a decode failure there answers with an exec-report
 *    reject, not 35=3.
 *  - Logon may carry the custom tag 20003 (CancelOnDisconnect=Y/N): wire
 *    negotiation of the session's cancel-on-disconnect (see
 *    docs/venue/perimeter.md, next to the 20001/20002 last-look tags). The
 *    gateway wires the negotiated value into the live session via
 *    setCodListener.
 *
 * State split: the outbound side (seq counter + event log) lives in
 * SessionRegistry::AccountStream; the inbound expectation lives in
 * FixSessionHost::AccountState. Both survive a disconnect. Across a process
 * restart, the SEQUENCE COUNTERS (per-account expectedIn + outbound lastSeq)
 * can be persisted into a sidecar file written at checkpoint time
 * (FixSessionSidecar; the gateway harness hooks
 * SequencedShard::onCheckpoint) and restored on startup -- then a Logon
 * without 141=Y that continues the pre-restart sequence space WORKS. The
 * EVENT LOG is deliberately not persisted: a resend that reaches into the
 * pre-restart range is answered with SequenceReset-GapFill -- the honest
 * signal for history the venue no longer holds; full state reconciliation is
 * the snapshot path. Without a sidecar the old rule stands: the first Logon
 * after a restart must carry 141=Y or it is answered with a Logout naming
 * the reason. See docs/venue/perimeter.md.
 *
 * Transport: FixConnection is transport-independent -- Tcp/Tls carry one FIX
 * message per length-prefixed frame (the framing is added by the writer's
 * WriteFn), Ws carries one FIX message per WebSocket Text frame (the framing
 * is added at enqueue time via setWireWrap, matching the gateway's
 * frame-at-encode model).
 */
#pragma once

#include "flox-venue/fix_codec.h"
#include "flox-venue/session_registry.h"

#include "flox/util/crc32.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

struct FixSessionConfig
{
  std::string senderCompId{"VENUE"};
  std::string targetCompId{"CLIENT"};
  uint32_t defaultHeartBtIntSec{30};       // until the Logon negotiates one
  int64_t logonTimeoutNs{10'000'000'000};  // no Logon within this -> disconnect
};

// Per-account FIX session state that OUTLIVES a connection (the inbound
// mirror of the outbound seq + log living in SessionRegistry::AccountStream):
// the expected inbound MsgSeqNum survives a disconnect so a reconnect without
// ResetSeqNumFlag resumes the same sequence space. Across a process restart
// it survives only through serialize()/restore() (the checkpoint sidecar,
// FixSessionSidecar) -- see the file comment.
class FixSessionHost
{
 public:
  explicit FixSessionHost(FixSessionConfig cfg = {}) : cfg_(std::move(cfg)) {}

  const FixSessionConfig& config() const noexcept { return cfg_; }

  struct AccountState
  {
    std::mutex m;
    uint64_t expectedIn{1};
    bool established{false};  // a Logon completed since process start (or restored state)
  };

  std::shared_ptr<AccountState> stateOf(uint64_t account)
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = states_.find(account);
    if (it != states_.end())
    {
      return it->second;
    }
    auto s = std::make_shared<AccountState>();
    states_.emplace(account, s);
    return s;
  }

  // ---- restart persistence (inbound side of the FIX sidecar) ----
  // Blob: [u32 count]{u64 account, u64 expectedIn}... -- one entry per
  // account that completed a Logon (only established sequence spaces are
  // worth carrying across a restart).
  void serialize(std::vector<uint8_t>& out)
  {
    std::vector<std::pair<uint64_t, uint64_t>> entries;
    {
      std::lock_guard<std::mutex> lk(m_);
      for (const auto& [account, state] : states_)
      {
        std::lock_guard<std::mutex> slk(state->m);
        if (state->established)
        {
          entries.emplace_back(account, state->expectedIn);
        }
      }
    }
    const uint32_t count = static_cast<uint32_t>(entries.size());
    append(out, &count, sizeof count);
    for (const auto& [account, expectedIn] : entries)
    {
      append(out, &account, sizeof account);
      append(out, &expectedIn, sizeof expectedIn);
    }
  }

  // Restore per-account expected inbound seqs into a fresh host (before any
  // connection is accepted). Restored accounts are marked established, so a
  // Logon that continues the pre-restart sequence space (no 141=Y) passes the
  // 34 check against the restored expectedIn. Returns bytes consumed, 0 on a
  // malformed blob.
  size_t restore(const uint8_t* p, size_t n)
  {
    uint32_t count = 0;
    if (n < sizeof count)
    {
      return 0;
    }
    std::memcpy(&count, p, sizeof count);
    const size_t need = sizeof count + static_cast<size_t>(count) * 16;
    if (n < need)
    {
      return 0;
    }
    const uint8_t* b = p + sizeof count;
    for (uint32_t i = 0; i < count; ++i, b += 16)
    {
      uint64_t account = 0;
      uint64_t expectedIn = 0;
      std::memcpy(&account, b, sizeof account);
      std::memcpy(&expectedIn, b + 8, sizeof expectedIn);
      auto s = stateOf(account);
      std::lock_guard<std::mutex> lk(s->m);
      s->expectedIn = expectedIn;
      s->established = true;
    }
    return need;
  }

 private:
  static void append(std::vector<uint8_t>& out, const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  }

  FixSessionConfig cfg_;
  std::mutex m_;
  std::unordered_map<uint64_t, std::shared_ptr<AccountState>> states_;
};

// The FIX session sidecar: per-account sequence counters (inbound expectedIn
// from FixSessionHost, outbound lastSeq from SessionRegistry) persisted next
// to the shard journal as `<base>.fixsessions`. Written by the gateway
// harness at every checkpoint boundary (SequencedShard::onCheckpoint) with
// the journal's durability discipline -- tmp file, fsync, atomic rename --
// and a trailing CRC32 over the whole payload (same integrity posture as a
// journal record: a torn or corrupted sidecar loads as "absent", which
// degrades to the old 141=Y requirement, never to a wrong sequence space).
// The event log is NOT in the sidecar (see the file comment: pre-restart
// ranges resend as GapFill).
class FixSessionSidecar
{
 public:
  static constexpr uint32_t kMagic = 0x46495853;  // "FIXS"

  static std::string pathFor(const std::string& journalBase) { return journalBase + ".fixsessions"; }

  static bool write(const std::string& path, FixSessionHost& host, SessionRegistry& registry)
  {
    std::vector<uint8_t> body;
    const uint32_t magic = kMagic;
    body.insert(body.end(), reinterpret_cast<const uint8_t*>(&magic),
                reinterpret_cast<const uint8_t*>(&magic) + sizeof magic);
    host.serialize(body);
    registry.serializeSeqs(body);
    const uint32_t crc = flox::util::Crc32::compute(body.data(), body.size());
    body.insert(body.end(), reinterpret_cast<const uint8_t*>(&crc),
                reinterpret_cast<const uint8_t*>(&crc) + sizeof crc);

    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
      return false;
    }
    size_t off = 0;
    while (off < body.size())
    {
      const ssize_t w = ::write(fd, body.data() + off, body.size() - off);
      if (w <= 0)
      {
        ::close(fd);
        return false;
      }
      off += static_cast<size_t>(w);
    }
    ::fsync(fd);
    ::close(fd);
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
  }

  // Load a sidecar into a fresh host + registry (before any connection is
  // accepted). False = absent / torn / corrupt: nothing is applied and the
  // session layer falls back to requiring 141=Y after the restart.
  static bool load(const std::string& path, FixSessionHost& host, SessionRegistry& registry)
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
      return false;
    }
    std::vector<uint8_t> body((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (body.size() < sizeof(uint32_t) * 2)
    {
      return false;
    }
    uint32_t crc = 0;
    std::memcpy(&crc, body.data() + body.size() - sizeof crc, sizeof crc);
    body.resize(body.size() - sizeof crc);
    if (flox::util::Crc32::compute(body.data(), body.size()) != crc)
    {
      return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, body.data(), sizeof magic);
    if (magic != kMagic)
    {
      return false;
    }
    const uint8_t* p = body.data() + sizeof magic;
    size_t left = body.size() - sizeof magic;
    const size_t usedHost = host.restore(p, left);
    if (usedHost == 0)
    {
      return false;
    }
    p += usedHost;
    left -= usedHost;
    return registry.restoreSeqs(p, left) != 0;
  }
};

// One live FIX connection. Owned by the gateway's connection loop: onFrame()
// gates every inbound message (session-layer traffic is consumed here;
// in-sequence application messages fall through to the normal decoder path),
// onTick() runs the heartbeat timers on the gateway's SO_RCVTIMEO idle tick.
// All outbound traffic goes through the SessionRegistry (seq allocation and
// enqueue are atomic on the account-stream mutex), so the single-writer
// contract of SessionWriter is preserved.
class FixConnection
{
 public:
  enum class Verdict : uint8_t
  {
    Handled,     // session-layer message, fully consumed
    App,         // in-sequence application message: decode + submit as usual
    Disconnect,  // session over (Logout exchanged / fatal): close the socket
  };

  // Transport framing applied to every outbound FIX message at enqueue time.
  // Default (unset) = the raw FIX bytes (Tcp/Tls: the writer's WriteFn adds
  // the length prefix). The WS gateway wraps each message in a WebSocket Text
  // frame here, because its registry carries fully-framed WS bytes.
  using WireWrap = std::function<std::vector<uint8_t>(const uint8_t*, size_t)>;
  // Wire-negotiated cancel-on-disconnect (Logon tag 20003): the gateway
  // applies the value to the live session + DisconnectCanceller.
  using CodListener = std::function<void(bool)>;

  FixConnection(FixSessionHost& host, SessionRegistry& registry, uint64_t account, int64_t nowNs)
      : cfg_(host.config()),
        registry_(registry),
        account_(account),
        state_(host.stateOf(account)),
        lastInNs_(nowNs),
        lastOutNs_(nowNs)
  {
    hbNs_ = static_cast<int64_t>(cfg_.defaultHeartBtIntSec) * 1'000'000'000LL;
  }

  void setWireWrap(WireWrap wrap) { wrap_ = std::move(wrap); }
  void setCodListener(CodListener l) { codListener_ = std::move(l); }

  Verdict onFrame(const std::string& msg, int64_t nowNs)
  {
    lastInNs_ = nowNs;
    testReqPending_ = false;  // any inbound traffic proves liveness
    if (!FixCodec::checksumValid(msg))
    {
      return Verdict::Handled;  // corrupt frame: drop; the seq gap machinery recovers the hole
    }
    auto f = FixCodec::parseFields(msg);
    const std::string type = f.count(35) != 0 ? f[35] : std::string{};
    if (!loggedOn_)
    {
      if (type != "A")
      {
        return logout("Logon required", nowNs);
      }
      return onLogon(f, nowNs);
    }
    const uint64_t seq = u64(f, 34);
    const bool possDup = f.count(43) != 0 && f[43] == "Y";

    std::lock_guard<std::mutex> lk(state_->m);
    if (type == "4")
    {
      // SequenceReset repairs the inbound sequence itself: handled before the
      // seq gate.
      onSequenceReset(f, seq, nowNs);
      return Verdict::Handled;
    }
    if (seq == 0)
    {
      return logout("MsgSeqNum (34) missing", nowNs);
    }
    if (seq < state_->expectedIn)
    {
      if (possDup)
      {
        return Verdict::Handled;  // already-seen PossDup replay: silent drop
      }
      return logout("MsgSeqNum too low, expected " + std::to_string(state_->expectedIn), nowNs);
    }
    if (seq > state_->expectedIn)
    {
      // Inbound gap: no reorder buffer -- request a resend and drop everything
      // above the hole (the peer replays it with PossDup); the request repeats
      // at most once per HeartBtInt. TestRequest is still answered so liveness
      // survives an open gap.
      requestResend(nowNs);
      if (type == "1")
      {
        heartbeatReply(f, nowNs);
      }
      return Verdict::Handled;
    }
    ++state_->expectedIn;
    gapPending_ = false;

    if (type == "0")
    {
      return Verdict::Handled;  // Heartbeat: liveness already refreshed above
    }
    if (type == "1")
    {
      heartbeatReply(f, nowNs);  // TestRequest -> Heartbeat echoing 112
      return Verdict::Handled;
    }
    if (type == "2")
    {
      serveResend(f, nowNs);
      return Verdict::Handled;
    }
    if (type == "5")
    {
      logoutSent_ = true;
      sendAdmin("5", {}, nowNs);  // confirming Logout; the loop exit runs COD
      return Verdict::Disconnect;
    }
    if (type == "A")
    {
      return Verdict::Handled;  // duplicate Logon on a live session: ignore
    }
    if (type == "D" || type == "F" || type == "G")
    {
      return Verdict::App;
    }
    // Unknown / unsupported MsgType: consumed in sequence, answered with a
    // session Reject (35=3) naming the offending seq and type -- never a
    // silent swallow, never a session kill.
    sendAdmin("3",
              {{45, std::to_string(seq)},
               {372, type.empty() ? std::string("?") : type},
               {58, "unsupported MsgType"}},
              nowNs);
    return Verdict::Handled;
  }

  // Timer pass on the gateway idle tick. False = liveness lost, disconnect.
  bool onTick(int64_t nowNs)
  {
    if (!loggedOn_)
    {
      return nowNs - lastInNs_ < cfg_.logonTimeoutNs;
    }
    if (nowNs - lastOutNs_ >= hbNs_)
    {
      sendAdmin("0", {}, nowNs);
    }
    const int64_t grace = hbNs_ + hbNs_ / 5;  // 1.2 * HeartBtInt
    if (nowNs - lastInNs_ >= grace)
    {
      if (!testReqPending_)
      {
        testReqPending_ = true;
        testReqNs_ = nowNs;
        sendAdmin("1", {{112, std::to_string(nowNs)}}, nowNs);
      }
      else if (nowNs - testReqNs_ >= grace)
      {
        return false;  // TestRequest unanswered for another 1.2 intervals
      }
    }
    return true;
  }

  bool loggedOn() const noexcept { return loggedOn_; }

 private:
  static uint64_t u64(std::unordered_map<int, std::string>& f, int tag)
  {
    return f.count(tag) != 0 ? std::strtoull(f[tag].c_str(), nullptr, 10) : 0;
  }

  Verdict onLogon(std::unordered_map<int, std::string>& f, int64_t nowNs)
  {
    if (f.count(108) != 0)
    {
      const long hb = std::atol(f[108].c_str());
      if (hb > 0)
      {
        hbNs_ = static_cast<int64_t>(hb) * 1'000'000'000LL;
      }
    }
    // Custom tag 20003 (CancelOnDisconnect=Y/N): per-session COD negotiation
    // on the wire. Applied on any accepted Logon, before order flow exists.
    const bool hasCod = f.count(20003) != 0;
    const bool codOn = hasCod && f[20003] == "Y";
    const auto applyCod = [&]
    {
      if (hasCod && codListener_)
      {
        codListener_(codOn);
      }
    };
    const uint64_t seq = u64(f, 34);
    const bool reset = f.count(141) != 0 && f[141] == "Y";
    std::lock_guard<std::mutex> lk(state_->m);
    if (reset)
    {
      // ResetSeqNumFlag: both directions restart at 1. The outbound stream
      // (seq counter + resend log) is cleared -- nothing from the previous
      // sequence space may replay into the new one.
      registry_.resetStream(account_);
      state_->expectedIn = (seq == 0 ? 1 : seq) + 1;
      state_->established = true;
      loggedOn_ = true;
      applyCod();
      logonReply(true, nowNs);
      return Verdict::Handled;
    }
    if (seq == 0)
    {
      return logout("MsgSeqNum (34) missing", nowNs);
    }
    if (seq < state_->expectedIn)
    {
      return logout("MsgSeqNum too low on Logon, expected " + std::to_string(state_->expectedIn),
                    nowNs);
    }
    if (seq > state_->expectedIn && !state_->established)
    {
      // In-memory session state was lost (venue restart): the counterparty
      // still runs the old sequence space and continuing would corrupt both
      // directions. Require an explicit reset.
      return logout("session state lost (venue restart): Logon must set ResetSeqNumFlag (141=Y)",
                    nowNs);
    }
    state_->established = true;
    loggedOn_ = true;
    applyCod();
    logonReply(false, nowNs);
    if (seq == state_->expectedIn)
    {
      ++state_->expectedIn;
    }
    else
    {
      requestResend(nowNs);  // their gap: recover it right after the Logon reply
    }
    return Verdict::Handled;
  }

  // state_->m held by the caller.
  void onSequenceReset(std::unordered_map<int, std::string>& f, uint64_t seq, int64_t nowNs)
  {
    const uint64_t newSeq = u64(f, 36);
    if (f.count(123) != 0 && f[123] == "Y")
    {
      if (seq > state_->expectedIn)
      {
        requestResend(nowNs);  // the GapFill itself sits beyond a hole
        return;
      }
      if (newSeq > state_->expectedIn)
      {
        state_->expectedIn = newSeq;
        gapPending_ = false;
      }
      return;  // stale / duplicate GapFill: ignore
    }
    // Reset mode (no 123=Y): hard sequence override. Accept it, loudly -- it
    // abandons messages without the GapFill accounting.
    std::fprintf(stderr,
                 "[fix] WARN account %llu: SequenceReset-Reset to %llu (expected %llu)\n",
                 static_cast<unsigned long long>(account_),
                 static_cast<unsigned long long>(newSeq),
                 static_cast<unsigned long long>(state_->expectedIn));
    if (newSeq != 0)
    {
      state_->expectedIn = newSeq;
      gapPending_ = false;
    }
  }

  // Their 35=2: replay [7=BeginSeqNo, 16=EndSeqNo] (16=0 -> everything).
  // Logged application events re-encode with PossDup + OrigSendingTime; seqs
  // absent from the log (admin traffic, trimmed history) collapse into
  // SequenceReset-GapFill. Replay frames carry ORIGINAL seqs and bypass seq
  // allocation (enqueueRaw).
  void serveResend(std::unordered_map<int, std::string>& f, int64_t nowNs)
  {
    const uint64_t last = registry_.lastSeq(account_);
    const uint64_t beginReq = u64(f, 7);
    const uint64_t endReq = u64(f, 16);
    const uint64_t begin = beginReq == 0 ? 1 : beginReq;
    const uint64_t end = (endReq == 0 || endReq > last) ? last : endReq;
    if (begin > end)
    {
      return;  // nothing in range (or nothing ever sent)
    }
    const std::string now52 = FixSession::sendingTime(nowNs);
    uint64_t cur = begin;
    for (const auto& logged : registry_.logSlice(account_, begin, end))
    {
      if (logged.seq > cur)
      {
        gapFill(cur, logged.seq, now52);
      }
      const std::string m =
          FixCodec::encode(logged.event, logged.seq, cfg_.senderCompId, cfg_.targetCompId, now52,
                           /*possDup*/ true, FixSession::sendingTime(logged.tsNs));
      if (!m.empty())
      {
        registry_.enqueueRaw(account_, toWire(m));
      }
      cur = logged.seq + 1;
    }
    if (cur <= end)
    {
      gapFill(cur, end + 1, now52);
    }
    registry_.noteResendServed();  // same counter as the SBE resend path
  }

  void gapFill(uint64_t seq, uint64_t newSeq, const std::string& now52)
  {
    const std::string m =
        FixCodec::encodeAdmin("4", seq, cfg_.senderCompId, cfg_.targetCompId, now52,
                              {{123, "Y"}, {36, std::to_string(newSeq)}}, /*possDup*/ true);
    registry_.enqueueRaw(account_, toWire(m));
  }

  // state_->m held by the caller.
  void requestResend(int64_t nowNs)
  {
    if (gapPending_ && nowNs - lastGapReqNs_ < hbNs_)
    {
      return;  // at most one 35=2 per HeartBtInt while the hole stays open
    }
    gapPending_ = true;
    lastGapReqNs_ = nowNs;
    sendAdmin("2", {{7, std::to_string(state_->expectedIn)}, {16, "0"}}, nowNs);
  }

  void heartbeatReply(std::unordered_map<int, std::string>& f, int64_t nowNs)
  {
    std::vector<std::pair<int, std::string>> fields;
    if (f.count(112) != 0)
    {
      fields.emplace_back(112, f[112]);  // TestReqID echo
    }
    sendAdmin("0", fields, nowNs);
  }

  void logonReply(bool reset, int64_t nowNs)
  {
    std::vector<std::pair<int, std::string>> fields{
        {108, std::to_string(hbNs_ / 1'000'000'000LL)}};
    if (reset)
    {
      fields.emplace_back(141, "Y");
    }
    sendAdmin("A", fields, nowNs);
  }

  Verdict logout(const std::string& text, int64_t nowNs)
  {
    if (!logoutSent_)
    {
      logoutSent_ = true;
      sendAdmin("5", {{58, text}}, nowNs);
    }
    return Verdict::Disconnect;
  }

  bool sendAdmin(const std::string& type, const std::vector<std::pair<int, std::string>>& fields,
                 int64_t nowNs)
  {
    lastOutNs_ = nowNs;
    return registry_.sendSequencedRaw(
        account_,
        [&](uint64_t seq, int64_t tsNs, std::vector<uint8_t>& out)
        {
          const std::string m = FixCodec::encodeAdmin(type, seq, cfg_.senderCompId,
                                                      cfg_.targetCompId,
                                                      FixSession::sendingTime(tsNs), fields);
          out = toWire(m);
          return true;
        });
  }

  // Transport framing for a raw FIX message (see setWireWrap).
  std::vector<uint8_t> toWire(const std::string& m) const
  {
    if (wrap_)
    {
      return wrap_(reinterpret_cast<const uint8_t*>(m.data()), m.size());
    }
    return std::vector<uint8_t>(m.begin(), m.end());
  }

  FixSessionConfig cfg_;
  SessionRegistry& registry_;
  uint64_t account_;
  std::shared_ptr<FixSessionHost::AccountState> state_;
  WireWrap wrap_;
  CodListener codListener_;
  int64_t hbNs_;
  int64_t lastInNs_;
  int64_t lastOutNs_;
  bool loggedOn_{false};
  bool logoutSent_{false};
  bool testReqPending_{false};
  int64_t testReqNs_{0};
  bool gapPending_{false};
  int64_t lastGapReqNs_{0};
};

}  // namespace flox::venue
