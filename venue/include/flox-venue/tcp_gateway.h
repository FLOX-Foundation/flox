/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/cancel_on_disconnect.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/messages.h"
#include "flox-venue/metrics.h"
#include "flox-venue/session.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/socket_acceptor.h"
#include "flox/util/transport.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace flox::venue
{

// Apply a receive timeout so a blocking read wakes up: liveness (idle peers)
// and shutdown (SocketAcceptor::stop's shutdown sweep) both depend on the
// read loop not being parked in the kernel forever.
inline void setRecvTimeoutMs(int fd, int64_t ms) noexcept
{
  if (ms <= 0)
  {
    return;
  }
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(ms / 1000);
  tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

inline bool wasRecvTimeout() noexcept { return errno == EAGAIN || errno == EWOULDBLOCK; }

class TcpGateway
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  using Handler = std::function<void(const InboundCommand&, const Responder&)>;

  // `account` is the account this endpoint serves. Every connection binds its
  // session to it, so a client cannot act as another account by writing a
  // different id into the payload (stampAccount forces the bound id). Run one
  // gateway per tenant, or extend with a logon handshake for multi-tenant.
  // account == 0 is the single-tenant-trusted mode: it trusts the
  // client-supplied accountId and must only face a trusted local producer.
  explicit TcpGateway(GatewaySession::Decoder decoder, uint64_t account = 0)
      : decoder_(std::move(decoder)), account_(account) {}

  // Gateway-wide default for NEW sessions; each session carries its own flag
  // (GatewaySession::setCancelOnDisconnect). Wire negotiation is future work.
  void setCancelOnDisconnect(bool on) noexcept { cancelOnDisconnect_.store(on); }

  // Delivery mode: register every connection in `registry` (keyed by the bound
  // account) and deliver exec reports through per-session bounded queues --
  // asynchronous events (maker fills, stop triggers, expiries, liquidations,
  // FillHeld) reach the session that owns them, and the matching thread never
  // blocks on a client socket. `encoder` serializes an event with its
  // per-session seq. Without a registry the gateway stays in the embedded
  // per-frame-responder mode (no async delivery, rejects silent).
  void setDelivery(SessionRegistry* registry, SessionRegistry::Encoder encoder)
  {
    registry_ = registry;
    encoder_ = std::move(encoder);
  }

  // Session-layer verbs (SBE resend/snapshot; see session_verbs.h). Only
  // consulted in delivery mode.
  void setSessionVerbs(SessionVerbHandler verbs) { verbs_ = std::move(verbs); }

  // Wire negotiation of per-session config (SBE SetSessionConfig -> COD; see
  // session_verbs.h makeSbeSessionConfigVerb). Fire-and-forget: the update is
  // applied to the live session, no reply frame.
  void setSessionConfigVerb(SessionConfigHandler h) { sessionCfg_ = std::move(h); }

  // FIX session layer (fix_session.h): every connection gets a FixConnection
  // that gates inbound frames (Logon, Heartbeat/TestRequest, resend both
  // ways) in front of the decoder and runs its timers on the idle tick.
  // Requires delivery mode (outbound goes through the SessionWriter queue and
  // the registry seq space); the encoder passed to setDelivery must produce
  // session-framed FIX. Frames are FIX tag=value messages carried in the
  // gateway's length-prefixed transport framing, one message per frame.
  void setFixSession(FixSessionHost* host) noexcept { fixHost_ = host; }

  void setCounters(GatewayCounters* counters) noexcept { counters_ = counters; }

  // Liveness: a session with no inbound bytes for this long is disconnected
  // (COD then fires normally). 0 disables.
  void setIdleTimeout(std::chrono::milliseconds t) noexcept { idleTimeoutMs_.store(t.count()); }

  int start(uint16_t port, Handler handler)
  {
    handler_ = std::move(handler);
    // Contain any exception to this one connection -- connLoop is a thread body,
    // so an escape would reach std::terminate and take down the whole venue.
    // The acceptor owns the fd (closes it after the handler returns).
    return acceptor_.start(port, [this](int fd)
                           {
                             try { connLoop(fd); }
                             catch (...) {} });
  }
  void stop() { acceptor_.stop(); }
  int port() const noexcept { return acceptor_.port(); }

 private:
  void connLoop(int fd)
  {
    GatewaySession session(account_, decoder_);
    session.authenticate(true);  // transport-level auth out of scope here
    session.setCancelOnDisconnect(cancelOnDisconnect_.load());
    DisconnectCanceller cod(session.cancelOnDisconnect());

    std::shared_ptr<SessionWriter> writer;
    if (registry_ != nullptr)
    {
      writer = registry_->attach(
          session.account(), encoder_,
          [fd](const uint8_t* p, size_t n)
          { return net::writeFrame(fd, p, n); },
          [fd]
          { ::shutdown(fd, SHUT_RDWR); },
          [&cod](const OutboundEvent& e)
          { cod.observe(e); });
    }
    // In delivery mode the responder feeds the same per-session queue as the
    // routed events, so a slow consumer can never stall the caller.
    const Responder responder =
        (writer != nullptr)
            ? Responder([w = writer.get()](const uint8_t* p, size_t n)
                        { w->enqueue(std::vector<uint8_t>(p, p + n)); })
            : Responder([fd](const uint8_t* p, size_t n)
                        { net::writeFrame(fd, p, n); });

    // FIX mode: the connection carries a FixConnection whose timers run on
    // the same SO_RCVTIMEO tick, just at heartbeat granularity instead of the
    // idle window (FIX liveness -- TestRequest death -- replaces the plain
    // idle disconnect).
    const bool fixMode = fixHost_ != nullptr && writer != nullptr;
    std::unique_ptr<FixConnection> fix;
    if (fixMode)
    {
      fix = std::make_unique<FixConnection>(*fixHost_, *registry_, session.account(),
                                            wallClockNs());
      // Logon tag 20003 (CancelOnDisconnect=Y/N) lands on this session.
      fix->setCodListener([&session, &cod](bool on)
                          {
                            session.setCancelOnDisconnect(on);
                            cod.setEnabled(on); });
    }
    const int64_t idleMs = idleTimeoutMs_.load();
    const int64_t tickMs = fixMode ? std::min<int64_t>(idleMs > 0 ? idleMs : 250, 250) : idleMs;
    setRecvTimeoutMs(fd, tickMs);

    std::vector<uint8_t> frame;
    while (acceptor_.running())
    {
      errno = 0;
      if (!net::readFrame(fd, frame))
      {
        if (wasRecvTimeout())
        {
          if (fixMode)
          {
            if (fix->onTick(wallClockNs()))
            {
              continue;  // timers serviced (heartbeat / test request), still live
            }
            // TestRequest went unanswered (or no Logon arrived): dead peer.
            if (counters_ != nullptr)
            {
              counters_->idleDisconnects.fetch_add(1, std::memory_order_relaxed);
            }
          }
          else if (idleMs > 0 && counters_ != nullptr)
          {
            // No inbound bytes for the whole idle window: half-open or wedged
            // peer. Close the session; COD sweeps its orders below.
            counters_->idleDisconnects.fetch_add(1, std::memory_order_relaxed);
          }
        }
        break;
      }
      if (fixMode)
      {
        const auto verdict =
            fix->onFrame(std::string(frame.begin(), frame.end()), wallClockNs());
        if (verdict == FixConnection::Verdict::Disconnect)
        {
          break;  // Logout exchanged / fatal session error; COD sweeps below
        }
        if (verdict == FixConnection::Verdict::Handled)
        {
          continue;  // session-layer message, fully consumed
        }
        // Verdict::App: an in-sequence application message -- fall through to
        // the decoder / admission path.
      }
      if (sessionCfg_)
      {
        if (const auto u = sessionCfg_(frame.data(), frame.size()))
        {
          // Session-config verb: applied in place, fire-and-forget (the
          // effect -- a disconnect sweeping or keeping orders -- is the ack).
          session.setCancelOnDisconnect(u->cancelOnDisconnect);
          cod.setEnabled(u->cancelOnDisconnect);
          continue;
        }
      }
      if (writer != nullptr && verbs_ &&
          verbs_(frame.data(), frame.size(), session.account(), *registry_))
      {
        continue;  // session-layer verb (resend / snapshot), fully handled
      }
      SessionReject rej{};
      // Real monotonic nanoseconds: the rate-limit windows are wall-clock. A
      // per-connection frame counter (the old ++clock_) never advanced time, so
      // the Nth command was limited regardless of elapsed time and the ban was
      // permanent.
      const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
      auto cmd = session.handle(frame.data(), frame.size(), nowNs, rej);
      if (cmd)
      {
        cod.track(*cmd);
        handler_(*cmd, responder);
      }
      else if (rej != SessionReject::None && registry_ != nullptr)
      {
        // A rejected frame answers with a sequenced exec-report reject (id 0:
        // for a non-decodable frame there is no order id to echo) instead of
        // the old silence.
        registry_->send(session.account(),
                        OutboundEvent{OrderRejected{0, 0, toRejectReason(rej),
                                                    session.account()}});
      }
    }
    if (writer != nullptr)
    {
      registry_->detach(session.account(), writer);
      writer->stop();
    }
    cod.flush(handler_);
    ::shutdown(fd, SHUT_RDWR);  // acceptor owns the close
  }

  GatewaySession::Decoder decoder_;
  uint64_t account_{0};
  Handler handler_;
  SocketAcceptor acceptor_;
  std::atomic<bool> cancelOnDisconnect_{false};
  SessionRegistry* registry_{nullptr};
  SessionRegistry::Encoder encoder_;
  SessionVerbHandler verbs_;
  SessionConfigHandler sessionCfg_;
  FixSessionHost* fixHost_{nullptr};
  GatewayCounters* counters_{nullptr};
  std::atomic<int64_t> idleTimeoutMs_{30'000};
};

}  // namespace flox::venue
