/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX 4.4 initiator: the client half of a session, and the mirror of the
 * acceptor in the venue module (venue/include/flox-venue/fix_session.h). The
 * two are written against each other, and the rules below are the acceptor's
 * rules seen from the other end -- deliberately, because a session where the
 * two ends disagree about recovery does not fail loudly, it delivers orders in
 * the wrong order.
 *
 *  - Logon (35=A) goes OUT: HeartBtInt (108) we propose, ResetSeqNumFlag
 *    (141=Y) when the sequence space starts fresh, and the custom tag 20003
 *    when the session negotiates cancel-on-disconnect on the wire. The reply
 *    adopts whatever HeartBtInt the acceptor answers with, because the
 *    acceptor's number is the one that governs.
 *  - Liveness: a Heartbeat goes out every HeartBtInt of outbound silence; no
 *    inbound traffic for 1.2 intervals sends a TestRequest (35=1); no answer
 *    for another 1.2 intervals ends the session. An inbound TestRequest is
 *    answered with a Heartbeat echoing 112.
 *  - Their ResendRequest (35=2): our application messages replay from the
 *    retained log, re-encoded with PossDupFlag (43=Y) and OrigSendingTime
 *    (122, the first transmission's 52) and their ORIGINAL 34. Seqs the log
 *    does not hold -- admin traffic, trimmed history -- collapse into
 *    SequenceReset-GapFill (35=4, 123=Y). Re-encoding rather than replaying
 *    bytes is not a choice: 43 and 122 change BodyLength and CheckSum.
 *  - Our inbound gap: no reorder buffer. We send 35=2 and DROP every message
 *    above the hole, repeating the request at most once per HeartBtInt, until
 *    the counterparty's PossDup replay closes it. The acceptor does the same,
 *    for the same reason: applying an execution report out of order tells the
 *    strategy a fill happened that has not happened yet, and the peer has to
 *    resend regardless.
 *  - Unknown MsgType: an in-sequence message whose 35 is outside what a client
 *    session speaks (admin 0/1/2/3/4/5/A/j, application 8/9) is answered --
 *    35=j when it is an application type we decline, 35=3 when the session
 *    layer could not make sense of it. Never a silent swallow.
 *  - Logout (35=5) carries its reason in 58, in both directions.
 *
 * Restart: the two sequence counters (our next outbound 34, the next inbound
 * 34 we expect) persist through FixInitiatorSidecar with the same durability
 * discipline the venue's sidecar uses -- tmp file, fsync, atomic rename,
 * trailing CRC32, and a torn file that loads as absent rather than as a wrong
 * sequence space. With a restored sidecar, a Logon without 141=Y continues the
 * pre-restart space and the acceptor accepts it. The resend LOG is not
 * persisted, so a ResendRequest reaching into the pre-restart range is answered
 * with GapFill -- the same honest signal the venue gives for history it no
 * longer holds.
 *
 * Transport: none. FixInitiator hands finished FIX messages to a SendFn and is
 * fed complete messages through onFrame(), so it runs over the framed TCP
 * client in fix_tcp_client.h, over TLS, over a WebSocket, or over a pair of
 * queues inside one process for a test.
 */
#pragma once

#include "flox/connector/fix/fix_client_codec.h"
#include "flox/connector/fix/fix_wire.h"
#include "flox/util/crc32.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace flox::fix
{

struct FixInitiatorConfig
{
  std::string senderCompId{"CLIENT"};
  std::string targetCompId{"VENUE"};
  uint32_t heartBtIntSec{30};
  bool resetSeqNumOnLogon{true};
  // Custom tag 20003 on the Logon (CancelOnDisconnect=Y/N). Unset leaves the
  // tag off and the acceptor's default stands.
  std::optional<bool> cancelOnDisconnect{};
  // No Logon reply within this and the session is over: an acceptor that
  // accepts the TCP connection and never answers is indistinguishable from a
  // hang otherwise.
  int64_t logonTimeoutNs{10'000'000'000};
  // How many application messages stay replayable for a counterparty's
  // ResendRequest. Older ones gap-fill.
  size_t resendLogCapacity{4096};
};

// The two counters that have to survive a restart, and nothing else.
struct FixSeqState
{
  uint64_t nextOut{1};     // the 34 our next outbound message carries
  uint64_t expectedIn{1};  // the 34 we require on the next inbound message
};

// Sequence counters on disk, next to whatever the caller keeps its state in.
// Layout: [u32 magic][u64 nextOut][u64 expectedIn][u32 crc32 over everything
// before it]. Written tmp -> fsync -> atomic rename, so a crash mid-write
// leaves the previous file intact rather than a half-written one; a torn or
// corrupt file loads as absent, which costs a 141=Y on the next Logon and
// never a wrong sequence space.
class FixInitiatorSidecar
{
 public:
  static constexpr uint32_t kMagic = 0x46495849;  // "FIXI"

  static bool write(const std::string& path, const FixSeqState& s)
  {
    std::vector<uint8_t> body;
    const uint32_t magic = kMagic;
    append(body, &magic, sizeof magic);
    append(body, &s.nextOut, sizeof s.nextOut);
    append(body, &s.expectedIn, sizeof s.expectedIn);
    const uint32_t crc = flox::util::Crc32::compute(body.data(), body.size());
    append(body, &crc, sizeof crc);

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

  // False = absent, short, torn or corrupt; `out` is left untouched.
  static bool load(const std::string& path, FixSeqState& out)
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
      return false;
    }
    std::vector<uint8_t> body((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    constexpr size_t kSize = sizeof(uint32_t) + sizeof(uint64_t) * 2 + sizeof(uint32_t);
    if (body.size() != kSize)
    {
      return false;
    }
    uint32_t crc = 0;
    std::memcpy(&crc, body.data() + body.size() - sizeof crc, sizeof crc);
    if (flox::util::Crc32::compute(body.data(), body.size() - sizeof crc) != crc)
    {
      return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, body.data(), sizeof magic);
    if (magic != kMagic)
    {
      return false;
    }
    FixSeqState s;
    std::memcpy(&s.nextOut, body.data() + 4, sizeof s.nextOut);
    std::memcpy(&s.expectedIn, body.data() + 12, sizeof s.expectedIn);
    if (s.nextOut == 0 || s.expectedIn == 0)
    {
      return false;  // FIX 4.4 sequence numbers start at 1; 0 is a corrupt file
    }
    out = s;
    return true;
  }

 private:
  static void append(std::vector<uint8_t>& out, const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  }
};

// Why the session is not up, for an operator paging on an incident and for
// an automaton that changes state on this flag -- the two audiences want
// different things from the same signal. A dropped TCP path recovers on its
// own; a rejected Logon does not, and retrying it unchanged just burns the
// same rejection again.
enum class SessionDownReason : uint8_t
{
  Connecting,       // Logon sent, no reply yet
  Refused,          // never reached a working session: the Logon could not be
                    // sent, the reply never came within logonTimeoutNs, or a
                    // pre-Logon reply made no sense
  LogonRejected,    // the counterparty answered Logon with an explicit
                    // Logout; text carries their reason (tag 58)
  Lost,             // the session was up and is now down: Logout exchanged,
                    // ours or theirs, or a sequence fault after Logon
  HeartbeatMissed,  // our TestRequest went unanswered past the grace window
};

// sessionDown() snapshot: the reason, when it last changed, and the last
// refusal/logout text seen for it (empty when the reason carries none).
// Safe to read from a thread other than the one driving onFrame()/onTick().
struct SessionDown
{
  SessionDownReason reason{SessionDownReason::Connecting};
  int64_t sinceNs{0};
  std::string text;
};

class FixInitiator
{
 public:
  enum class Verdict : uint8_t
  {
    Handled,     // consumed at the session layer, or dispatched as a report
    Disconnect,  // the session is over: close the socket
  };

  // Hands one finished FIX message to the transport. False = the write failed.
  using SendFn = std::function<bool(const std::string&)>;
  // One decoded application message from the counterparty.
  using ReportFn = std::function<void(const InboundReport&)>;
  // Called whenever the persisted counters move, so the caller can checkpoint
  // them at a boundary that means something to it rather than on every message.
  using SeqObserver = std::function<void(const FixSeqState&)>;

  explicit FixInitiator(FixInitiatorConfig cfg = {}) : cfg_(std::move(cfg))
  {
    hbNs_ = static_cast<int64_t>(cfg_.heartBtIntSec) * 1'000'000'000LL;
  }

  void setSend(SendFn fn) { send_ = std::move(fn); }
  void setReportHandler(ReportFn fn) { onReport_ = std::move(fn); }
  void setSeqObserver(SeqObserver fn) { onSeq_ = std::move(fn); }

  // Resume a sequence space restored from a sidecar. Call before connect();
  // it also turns the ResetSeqNumFlag off, since the point of restoring the
  // counters is to continue the space the counterparty still believes in.
  void restore(const FixSeqState& s)
  {
    std::lock_guard<std::mutex> lk(m_);
    seq_ = s;
    cfg_.resetSeqNumOnLogon = false;
  }

  FixSeqState seqState()
  {
    std::lock_guard<std::mutex> lk(m_);
    return seq_;
  }

  // Why the session is currently not up -- irrelevant while loggedOn() is
  // true, since loggedOn() is the up/down signal proper; this just explains
  // a false answer to an operator or a reconnect policy. Safe from another
  // thread the same way seqState() is.
  SessionDown sessionDown()
  {
    std::lock_guard<std::mutex> lk(m_);
    return sessionDown_;
  }

  const FixInitiatorConfig& config() const noexcept { return cfg_; }
  bool loggedOn() const noexcept { return loggedOn_; }
  uint32_t negotiatedHeartBtIntSec() const noexcept
  {
    return static_cast<uint32_t>(hbNs_ / 1'000'000'000LL);
  }

  // Open the session: send the Logon. Call once per TCP connection, after the
  // transport is up.
  bool connect(int64_t nowNs)
  {
    std::lock_guard<std::mutex> lk(m_);
    loggedOn_ = false;
    logoutSent_ = false;
    testReqPending_ = false;
    gapPending_ = false;
    lastInNs_ = nowNs;
    lastOutNs_ = nowNs;
    logonSentNs_ = nowNs;
    if (cfg_.resetSeqNumOnLogon)
    {
      // 141=Y restarts BOTH directions at 1, so the retained log of the old
      // space must go with it: nothing from before the reset may replay into
      // the new sequence space.
      seq_ = FixSeqState{};
      log_.clear();
    }
    std::vector<std::pair<int, std::string>> fields{
        {98, "0"},  // EncryptMethod: none. Transport-level TLS is not this tag.
        {108, std::to_string(cfg_.heartBtIntSec)}};
    if (cfg_.resetSeqNumOnLogon)
    {
      fields.emplace_back(141, "Y");
    }
    if (cfg_.cancelOnDisconnect.has_value())
    {
      fields.emplace_back(20003, *cfg_.cancelOnDisconnect ? "Y" : "N");
    }
    const bool sent = sendAdminLocked("A", fields, nowNs);
    // A Logon that never left is the same "unreachable" an operator means by
    // a dead socket; one that went out just has no reply yet.
    setSessionDown(sent ? SessionDownReason::Connecting : SessionDownReason::Refused,
                   sent ? std::string{} : std::string{"Logon could not be sent"}, nowNs);
    return sent;
  }

  // ---- application sends ----
  // Each allocates the next outbound 34, encodes, retains the bare body for a
  // future resend, and writes. False = not logged on, or the write failed.
  bool submit(const NewOrderRequest& o, int64_t nowNs) { return sendApp(ClientCodec::encode(o), nowNs); }
  bool cancel(const CancelRequest& c, int64_t nowNs) { return sendApp(ClientCodec::encode(c), nowNs); }
  bool replace(const CancelReplaceRequest& r, int64_t nowNs)
  {
    return sendApp(ClientCodec::encode(r), nowNs);
  }

  // Close the session with a reason the counterparty can read in tag 58.
  bool logout(const std::string& reason, int64_t nowNs)
  {
    std::lock_guard<std::mutex> lk(m_);
    return logoutLocked(reason, nowNs);
  }

  // One complete inbound FIX message.
  Verdict onFrame(const std::string& msg, int64_t nowNs)
  {
    InboundReport report;
    bool haveReport = false;
    Verdict v = Verdict::Handled;
    {
      std::lock_guard<std::mutex> lk(m_);
      v = onFrameLocked(msg, nowNs, report, haveReport);
    }
    // Dispatched outside the lock: the handler is strategy code, and holding a
    // session mutex across it would let a slow strategy stall the heartbeat.
    if (haveReport && onReport_)
    {
      onReport_(report);
    }
    return v;
  }

  // Timer pass, on whatever idle tick the transport provides. False = the
  // session is dead and the caller should close the socket.
  bool onTick(int64_t nowNs)
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!loggedOn_)
    {
      if (nowNs - logonSentNs_ < cfg_.logonTimeoutNs)
      {
        return true;
      }
      setSessionDown(SessionDownReason::Refused, "Logon reply timed out", nowNs);
      return false;
    }
    if (nowNs - lastOutNs_ >= hbNs_)
    {
      sendAdminLocked("0", {}, nowNs);
    }
    const int64_t grace = hbNs_ + hbNs_ / 5;  // 1.2 * HeartBtInt
    if (nowNs - lastInNs_ >= grace)
    {
      if (!testReqPending_)
      {
        testReqPending_ = true;
        testReqNs_ = nowNs;
        sendAdminLocked("1", {{112, std::to_string(nowNs)}}, nowNs);
      }
      else if (nowNs - testReqNs_ >= grace)
      {
        setSessionDown(SessionDownReason::HeartbeatMissed, "TestRequest unanswered", nowNs);
        return false;  // TestRequest unanswered for another 1.2 intervals
      }
    }
    return true;
  }

 private:
  struct Logged
  {
    uint64_t seq;
    std::string bare;  // the bare encoding, header injected fresh at each send
    int64_t tsNs;      // first transmission, for OrigSendingTime (122)
  };

  Verdict onFrameLocked(const std::string& msg, int64_t nowNs, InboundReport& report,
                        bool& haveReport)
  {
    lastInNs_ = nowNs;
    testReqPending_ = false;  // any inbound traffic proves liveness
    if (!checksumValid(msg))
    {
      // A corrupt frame is dropped, not answered: we cannot trust its 34
      // either, and the gap machinery recovers the hole it leaves.
      return Verdict::Handled;
    }
    Fields f = parseFields(msg);
    const std::string type = fieldStr(f, 35);
    if (!loggedOn_)
    {
      if (type == "5")
      {
        // Logon refused, reason in 58: an explicit answer, not a timeout or a
        // dead transport, so it gets its own reason rather than Refused.
        setSessionDown(SessionDownReason::LogonRejected, fieldStr(f, 58), nowNs);
        return Verdict::Disconnect;
      }
      if (type != "A")
      {
        // Anything before the Logon reply is out of place. Say so and go.
        return logoutLocked("Logon reply expected", nowNs) ? Verdict::Disconnect
                                                           : Verdict::Disconnect;
      }
      return onLogonReply(f, nowNs);
    }

    const uint64_t seq = fieldU64(f, 34);
    const bool possDup = fieldStr(f, 43) == "Y";

    if (type == "4")
    {
      onSequenceReset(f, seq, nowNs);  // repairs the sequence itself: before the gate
      return Verdict::Handled;
    }
    if (seq == 0)
    {
      logoutLocked("MsgSeqNum (34) missing", nowNs);
      return Verdict::Disconnect;
    }
    if (seq < seq_.expectedIn)
    {
      if (possDup)
      {
        return Verdict::Handled;  // already-seen PossDup replay: silent drop
      }
      logoutLocked("MsgSeqNum too low, expected " + std::to_string(seq_.expectedIn), nowNs);
      return Verdict::Disconnect;
    }
    if (seq > seq_.expectedIn)
    {
      // The hole. Ask for a replay and drop everything above it; a TestRequest
      // is still answered so liveness survives an open gap.
      requestResend(nowNs);
      if (type == "1")
      {
        heartbeatReply(f, nowNs);
      }
      return Verdict::Handled;
    }
    ++seq_.expectedIn;
    gapPending_ = false;
    noteSeq();

    if (type == "0")
    {
      return Verdict::Handled;  // Heartbeat: liveness already refreshed
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
      if (!logoutSent_)
      {
        logoutSent_ = true;
        sendAdminLocked("5", {}, nowNs);  // confirming Logout
      }
      // The session was up: this is a session loss, not a Logon-time refusal.
      setSessionDown(SessionDownReason::Lost, fieldStr(f, 58), nowNs);
      return Verdict::Disconnect;
    }
    if (type == "A")
    {
      return Verdict::Handled;  // duplicate Logon on a live session: ignore
    }
    if (type == "8" || type == "9" || type == "3" || type == "j")
    {
      auto decoded = ClientCodec::decode(msg);
      if (decoded)
      {
        report = *decoded;
        haveReport = true;
      }
      return Verdict::Handled;
    }
    // In sequence, and outside what a client session speaks. Answered, never
    // swallowed, and the answer distinguishes the two cases the way the
    // acceptor does: 35=j is "the session is fine, we decline this message",
    // 35=3 is "the session layer could not process this".
    if (isApplicationMsgType(type))
    {
      sendAdminLocked("j",
                      {{45, std::to_string(seq)},
                       {372, type},
                       {380, "3"},  // BusinessRejectReason = Unsupported Message Type
                       {58, "message type not supported by this client session"}},
                      nowNs);
      return Verdict::Handled;
    }
    sendAdminLocked("3",
                    {{45, std::to_string(seq)},
                     {372, type.empty() ? std::string("?") : type},
                     {373, "11"},  // SessionRejectReason = Invalid MsgType
                     {58, "invalid MsgType"}},
                    nowNs);
    return Verdict::Handled;
  }

  Verdict onLogonReply(Fields& f, int64_t nowNs)
  {
    // The acceptor's HeartBtInt governs: it answers with the interval it will
    // actually keep, which may not be the one we proposed.
    const std::string hbStr = fieldStr(f, 108);
    if (!hbStr.empty())
    {
      const long hb = std::atol(hbStr.c_str());
      if (hb > 0)
      {
        hbNs_ = static_cast<int64_t>(hb) * 1'000'000'000LL;
      }
    }
    const uint64_t seq = fieldU64(f, 34);
    const bool theyReset = fieldStr(f, 141) == "Y";
    loggedOn_ = true;
    if (theyReset)
    {
      // They restarted their outbound direction. Take their 34 as the truth
      // about where their space now is, rather than insisting on ours.
      seq_.expectedIn = (seq == 0 ? 1 : seq) + 1;
      noteSeq();
      return Verdict::Handled;
    }
    if (seq == 0)
    {
      logoutLocked("MsgSeqNum (34) missing on Logon reply", nowNs);
      return Verdict::Disconnect;
    }
    if (seq < seq_.expectedIn)
    {
      logoutLocked("MsgSeqNum too low on Logon reply, expected " + std::to_string(seq_.expectedIn),
                   nowNs);
      return Verdict::Disconnect;
    }
    if (seq == seq_.expectedIn)
    {
      ++seq_.expectedIn;
      noteSeq();
    }
    else
    {
      requestResend(nowNs);  // their gap: recover it right after the Logon reply
    }
    return Verdict::Handled;
  }

  void onSequenceReset(Fields& f, uint64_t seq, int64_t nowNs)
  {
    const uint64_t newSeq = fieldU64(f, 36);
    if (fieldStr(f, 123) == "Y")
    {
      if (seq > seq_.expectedIn)
      {
        requestResend(nowNs);  // the GapFill itself sits beyond a hole
        return;
      }
      if (newSeq > seq_.expectedIn)
      {
        seq_.expectedIn = newSeq;
        gapPending_ = false;
        noteSeq();
      }
      return;  // stale or duplicate GapFill: ignore
    }
    // Reset mode (no 123=Y): a hard override that abandons messages without the
    // GapFill accounting. Accepted, because refusing it strands the session.
    if (newSeq != 0)
    {
      seq_.expectedIn = newSeq;
      gapPending_ = false;
      noteSeq();
    }
  }

  // Their 35=2: replay [7=BeginSeqNo, 16=EndSeqNo] (16=0 means everything).
  // Logged application messages re-encode with 43=Y + 122 and their ORIGINAL
  // 34; seqs the log does not hold collapse into SequenceReset-GapFill.
  void serveResend(Fields& f, int64_t nowNs)
  {
    const uint64_t last = seq_.nextOut - 1;
    const uint64_t beginReq = fieldU64(f, 7);
    const uint64_t endReq = fieldU64(f, 16);
    const uint64_t begin = beginReq == 0 ? 1 : beginReq;
    const uint64_t end = (endReq == 0 || endReq > last) ? last : endReq;
    if (begin > end || last == 0)
    {
      return;  // nothing in range, or nothing ever sent
    }
    const std::string now52 = sendingTime(nowNs);
    uint64_t cur = begin;
    for (const Logged& l : log_)
    {
      if (l.seq < begin || l.seq > end)
      {
        continue;
      }
      if (l.seq > cur)
      {
        gapFill(cur, l.seq, now52);
      }
      const std::string m = ClientCodec::reframe(l.bare, l.seq, cfg_.senderCompId,
                                                 cfg_.targetCompId, now52,
                                                 /*possDup*/ true, sendingTime(l.tsNs));
      if (!m.empty() && send_)
      {
        send_(m);
      }
      cur = l.seq + 1;
    }
    if (cur <= end)
    {
      gapFill(cur, end + 1, now52);
    }
    lastOutNs_ = nowNs;
  }

  // A GapFill covers [seq, newSeq) and carries the ORIGINAL seq of the first
  // message in the range, so it does not consume a new outbound number.
  void gapFill(uint64_t seq, uint64_t newSeq, const std::string& now52)
  {
    const std::string m = encodeAdmin("4", seq, cfg_.senderCompId, cfg_.targetCompId, now52,
                                      {{123, "Y"}, {36, std::to_string(newSeq)}}, /*possDup*/ true);
    if (send_)
    {
      send_(m);
    }
  }

  void requestResend(int64_t nowNs)
  {
    if (gapPending_ && nowNs - lastGapReqNs_ < hbNs_)
    {
      return;  // at most one 35=2 per HeartBtInt while the hole stays open
    }
    gapPending_ = true;
    lastGapReqNs_ = nowNs;
    sendAdminLocked("2", {{7, std::to_string(seq_.expectedIn)}, {16, "0"}}, nowNs);
  }

  void heartbeatReply(Fields& f, int64_t nowNs)
  {
    std::vector<std::pair<int, std::string>> fields;
    const std::string id = fieldStr(f, 112);
    if (!id.empty())
    {
      fields.emplace_back(112, id);  // TestReqID echo
    }
    sendAdminLocked("0", fields, nowNs);
  }

  bool logoutLocked(const std::string& reason, int64_t nowNs)
  {
    if (logoutSent_)
    {
      return true;
    }
    logoutSent_ = true;
    // A session that never reached loggedOn_ was never up to lose: whatever
    // sent us here before that point is a Refused-class failure to raise the
    // session, not a Lost one. Once up, our own Logout is as much a loss of
    // the session as one the counterparty sends.
    setSessionDown(loggedOn_ ? SessionDownReason::Lost : SessionDownReason::Refused, reason, nowNs);
    return sendAdminLocked("5", {{58, reason}}, nowNs);
  }

  bool sendAdminLocked(const std::string& type,
                       const std::vector<std::pair<int, std::string>>& fields, int64_t nowNs)
  {
    if (!send_)
    {
      return false;
    }
    const uint64_t s = seq_.nextOut++;
    noteSeq();
    lastOutNs_ = nowNs;
    // Admin messages are sequenced but NOT logged. A counterparty asking for
    // one back gets a GapFill, which is what FIX says a Heartbeat's seq is
    // worth on replay.
    return send_(encodeAdmin(type, s, cfg_.senderCompId, cfg_.targetCompId, sendingTime(nowNs),
                             fields));
  }

  bool sendApp(const std::string& bare, int64_t nowNs)
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!loggedOn_ || !send_ || bare.empty())
    {
      return false;
    }
    const uint64_t s = seq_.nextOut++;
    noteSeq();
    lastOutNs_ = nowNs;
    log_.push_back(Logged{s, bare, nowNs});
    while (log_.size() > cfg_.resendLogCapacity)
    {
      log_.pop_front();
    }
    return send_(ClientCodec::reframe(bare, s, cfg_.senderCompId, cfg_.targetCompId,
                                      sendingTime(nowNs)));
  }

  void noteSeq()
  {
    if (onSeq_)
    {
      onSeq_(seq_);
    }
  }

  void setSessionDown(SessionDownReason reason, const std::string& text, int64_t nowNs)
  {
    // Every path that reports why the session is down is, definitionally, a
    // path where the session is no longer up. Clearing loggedOn_ here --
    // instead of at each of the four call sites (EOF/write failure via
    // logout(), HeartbeatMissed, counterparty Logout) -- is what keeps a
    // future fifth path from repeating the same miss: a caller judging
    // reachability by loggedOn() would otherwise keep sending into a socket
    // this object already knows is gone.
    loggedOn_ = false;
    sessionDown_ = SessionDown{reason, nowNs, text};
  }

  static std::string fieldStr(Fields& f, int tag)
  {
    const auto it = f.find(tag);
    return it != f.end() ? it->second : std::string{};
  }

  static uint64_t fieldU64(Fields& f, int tag)
  {
    const auto it = f.find(tag);
    return it != f.end() ? std::strtoull(it->second.c_str(), nullptr, 10) : 0;
  }

  FixInitiatorConfig cfg_;
  SendFn send_;
  ReportFn onReport_;
  SeqObserver onSeq_;

  std::mutex m_;
  FixSeqState seq_;
  std::deque<Logged> log_;
  SessionDown sessionDown_;

  int64_t hbNs_{0};
  int64_t lastInNs_{0};
  int64_t lastOutNs_{0};
  int64_t logonSentNs_{0};
  int64_t testReqNs_{0};
  int64_t lastGapReqNs_{0};
  bool loggedOn_{false};
  bool logoutSent_{false};
  bool testReqPending_{false};
  bool gapPending_{false};
};

}  // namespace flox::fix
