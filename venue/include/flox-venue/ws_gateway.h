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
#include "flox-venue/tcp_gateway.h"  // setRecvTimeoutMs / wasRecvTimeout
#include "flox/util/transport.h"
#include "flox/util/websocket.h"

#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace flox::venue
{

class WsGateway
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  using Handler = DisconnectCanceller::Handler;  // (cmd, responder, recvMonoNs)

  // See TcpGateway: `account` binds each session so a client cannot spoof
  // another account's id. account == 0 is single-tenant-trusted mode.
  explicit WsGateway(GatewaySession::Decoder decoder, uint64_t account = 0)
      : decoder_(std::move(decoder)), account_(account) {}

  void setCancelOnDisconnect(bool on) noexcept { cancelOnDisconnect_.store(on); }

  // Delivery mode (see TcpGateway::setDelivery). The registry carries
  // fully-framed WebSocket bytes: the gateway wraps `encoder`'s payload in a
  // Text frame, so the single writer thread owns the socket and control
  // frames never interleave mid-message.
  void setDelivery(SessionRegistry* registry, SessionRegistry::Encoder encoder)
  {
    registry_ = registry;
    encoder_ = std::move(encoder);
  }

  void setCounters(GatewayCounters* counters) noexcept { counters_ = counters; }

  // Wire negotiation of per-session config (SBE SetSessionConfig -> COD; see
  // session_verbs.h makeSbeSessionConfigVerb). Fire-and-forget.
  void setSessionConfigVerb(SessionConfigHandler h) { sessionCfg_ = std::move(h); }

  // FIX session layer over WebSocket (fix_session.h; requires delivery mode).
  // FIX messages travel ONE PER WebSocket data frame: the venue sends Text
  // frames (FIX tag=value is ASCII; Text keeps the messages readable in any
  // WS tooling) and accepts inbound FIX in Text or Binary frames alike --
  // both carry the same payload bytes and RFC 6455 imposes no semantics
  // beyond UTF-8 validity, which FIX satisfies. Fragmented data frames
  // reassemble as usual before the session layer sees the message. FIX
  // liveness (Heartbeat/TestRequest death) replaces the WS Ping probe on
  // these connections; the timers run on the same recv-timeout tick at
  // heartbeat granularity.
  void setFixSession(FixSessionHost* host) noexcept { fixHost_ = host; }

  // Liveness: the server pings at half this window; a peer with no inbound
  // bytes (data or Pong) for the whole window is disconnected. 0 disables.
  void setIdleTimeout(std::chrono::milliseconds t) noexcept { idleTimeoutMs_.store(t.count()); }

  int start(uint16_t port, Handler handler)
  {
    handler_ = std::move(handler);
    // connLoop runs as a per-connection thread body: an escaping exception would
    // reach std::terminate and kill the whole venue, so contain it to this one
    // connection (the acceptor closes the fd and the thread exits cleanly).
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
    const int64_t idleMs = idleTimeoutMs_.load();
    setRecvTimeoutMs(fd, idleMs > 0 ? std::max<int64_t>(idleMs / 2, 1) : 0);

    // 1. HTTP Upgrade handshake.
    std::string req;
    uint8_t tmp[2048];
    while (req.find("\r\n\r\n") == std::string::npos)
    {
      const ssize_t r = ::read(fd, tmp, sizeof tmp);
      if (r <= 0 || req.size() > (1u << 16))
      {
        return;
      }
      req.append(reinterpret_cast<char*>(tmp), static_cast<size_t>(r));
    }
    const std::string resp = ws::handshakeResponse(req);
    if (resp.empty())
    {
      return;
    }
    net::writeAll(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());

    // 2. Frame loop.
    GatewaySession session(account_, decoder_);
    session.authenticate(true);
    session.setCancelOnDisconnect(cancelOnDisconnect_.load());
    DisconnectCanceller cod(session.cancelOnDisconnect());

    std::shared_ptr<SessionWriter> writer;
    if (registry_ != nullptr)
    {
      // The registry logs/queues COMPLETE WebSocket frames so replay and live
      // delivery are byte-identical and only the writer thread touches the fd.
      SessionRegistry::Encoder wsEncoder =
          [enc = encoder_](const OutboundEvent& e, uint64_t seq, int64_t tsNs,
                           std::vector<uint8_t>& out) -> bool
      {
        std::vector<uint8_t> payload;
        if (!enc || !enc(e, seq, tsNs, payload))
        {
          return false;
        }
        out = ws::buildFrame(ws::Opcode::Text, payload.data(), payload.size());
        return true;
      };
      writer = registry_->attach(
          session.account(), std::move(wsEncoder),
          [fd](const uint8_t* p, size_t n)
          { return net::writeAll(fd, p, n); },
          [fd]
          { ::shutdown(fd, SHUT_RDWR); },
          [&cod](const OutboundEvent& e)
          { cod.observe(e); });
    }
    const Responder responder =
        (writer != nullptr)
            ? Responder([w = writer.get()](const uint8_t* p, size_t n)
                        { w->enqueue(ws::buildFrame(ws::Opcode::Text, p, n)); })
            : Responder([fd](const uint8_t* p, size_t n)
                        {
                          const auto f = ws::buildFrame(ws::Opcode::Text, p, n);
                          net::writeAll(fd, f.data(), f.size()); });
    // Single-writer rule: with a writer thread every outbound frame (including
    // Pong/Ping control frames) goes through the queue; without one the read
    // thread is the only writer and may write directly.
    const auto sendControl = [&](std::vector<uint8_t> f)
    {
      if (writer != nullptr)
      {
        writer->enqueue(std::move(f));
      }
      else
      {
        net::writeAll(fd, f.data(), f.size());
      }
    };

    // FIX mode (see setFixSession): the FixConnection gates every data
    // message; its outbound raw messages (admin, PossDup replays) wrap into
    // WS Text frames at enqueue time, matching the frame-at-encode model of
    // this gateway's registry encoder. Timers run on a heartbeat-granularity
    // recv-timeout tick instead of the half-idle Ping probe.
    const bool fixMode = fixHost_ != nullptr && writer != nullptr;
    std::unique_ptr<FixConnection> fix;
    if (fixMode)
    {
      fix = std::make_unique<FixConnection>(*fixHost_, *registry_, session.account(),
                                            wallClockNs());
      fix->setWireWrap([](const uint8_t* p, size_t n)
                       { return ws::buildFrame(ws::Opcode::Text, p, n); });
      fix->setCodListener([&session, &cod](bool on)
                          {
                            session.setCancelOnDisconnect(on);
                            cod.setEnabled(on); });
      setRecvTimeoutMs(fd, idleMs > 0 ? std::min<int64_t>(idleMs, 250) : 250);
    }

    const auto now = []
    { return std::chrono::steady_clock::now(); };
    auto lastInbound = now();
    bool idleTripped = false;

    // One fully-assembled data message: session-config verb, then the FIX
    // session gate (in FIX mode), then the normal decoder path. False = tear
    // the connection down.
    const auto onMessage = [&](const uint8_t* p, size_t n) -> bool
    {
      if (sessionCfg_)
      {
        if (const auto u = sessionCfg_(p, n))
        {
          session.setCancelOnDisconnect(u->cancelOnDisconnect);
          cod.setEnabled(u->cancelOnDisconnect);
          return true;
        }
      }
      if (fixMode)
      {
        const auto verdict =
            fix->onFrame(std::string(reinterpret_cast<const char*>(p), n), wallClockNs());
        if (verdict == FixConnection::Verdict::Disconnect)
        {
          return false;  // Logout exchanged / fatal session error; COD sweeps below
        }
        if (verdict == FixConnection::Verdict::Handled)
        {
          return true;  // session-layer message, fully consumed
        }
        // Verdict::App: fall through to the decoder / admission path.
      }
      dispatch(session, cod, responder, p, n);
      return true;
    };

    std::vector<uint8_t> buf;
    // Fragmentation reassembly (RFC 6455 5.4): a data message may span a
    // FIN=0 Text/Binary frame followed by Continuation frames; control frames
    // (Ping/Pong/Close) may interleave and are handled immediately.
    std::vector<uint8_t> msg;
    ws::Opcode msgOp{};
    bool assembling = false;
    while (acceptor_.running())
    {
      ws::Opcode op{};
      std::vector<uint8_t> payload;
      bool fin = true;
      const size_t consumed = ws::parseFrame(buf.data(), buf.size(), op, payload, &fin);
      if (consumed == ws::kParseError)
      {
        break;  // hostile/oversized/malformed frame: drop the connection
      }
      if (consumed == 0)
      {
        errno = 0;
        const ssize_t r = ::read(fd, tmp, sizeof tmp);
        if (r > 0)
        {
          lastInbound = now();
          buf.insert(buf.end(), tmp, tmp + r);
          continue;
        }
        if (r < 0 && wasRecvTimeout())
        {
          if (fixMode)
          {
            // FIX liveness replaces the WS Ping probe: heartbeat timers run
            // here; an unanswered TestRequest is the dead-peer signal.
            if (fix->onTick(wallClockNs()))
            {
              continue;
            }
            idleTripped = true;
            break;
          }
          if (idleMs > 0)
          {
            // Half the idle window elapsed with no bytes. Past the full
            // window: the peer is gone or wedged -- disconnect (COD fires
            // below). Otherwise probe with a Ping; any inbound (Pong or data)
            // before the deadline keeps the session alive.
            const auto idle =
                std::chrono::duration_cast<std::chrono::milliseconds>(now() - lastInbound).count();
            if (idle >= idleMs)
            {
              idleTripped = true;
              break;
            }
            sendControl(ws::buildFrame(ws::Opcode::Ping, nullptr, 0));
            continue;
          }
        }
        break;  // EOF / error / shutdown sweep
      }
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
      lastInbound = now();

      // Control frames are always final and may arrive mid-message.
      if (op == ws::Opcode::Close)
      {
        break;
      }
      if (op == ws::Opcode::Ping)
      {
        sendControl(ws::buildFrame(ws::Opcode::Pong, payload.data(), payload.size()));
        continue;
      }
      if (op == ws::Opcode::Pong)
      {
        continue;
      }

      // Data frames: Text/Binary start a message, Continuation extends it.
      if (op == ws::Opcode::Text || op == ws::Opcode::Binary)
      {
        if (assembling)
        {
          break;  // new data frame before the previous message finished: violation
        }
        if (fin)
        {
          if (!onMessage(payload.data(), payload.size()))
          {
            break;
          }
          continue;
        }
        assembling = true;
        msgOp = op;
        msg = std::move(payload);
        continue;
      }
      if (op == ws::Opcode::Cont)
      {
        if (!assembling)
        {
          break;  // continuation with no message in progress: violation
        }
        if (msg.size() + payload.size() > ws::kMaxFramePayload)
        {
          break;  // reassembled message exceeds the bound
        }
        msg.insert(msg.end(), payload.begin(), payload.end());
        if (fin)
        {
          const bool keep = onMessage(msg.data(), msg.size());
          assembling = false;
          msg.clear();
          if (!keep)
          {
            break;
          }
        }
        continue;
      }
      break;  // unknown opcode
    }
    if (idleTripped && counters_ != nullptr)
    {
      counters_->idleDisconnects.fetch_add(1, std::memory_order_relaxed);
    }
    if (writer != nullptr)
    {
      registry_->detach(session.account(), writer);
      writer->stop();
    }
    cod.flush(handler_);
    ::shutdown(fd, SHUT_RDWR);  // acceptor owns the close
  }

  // Decode one fully-assembled message and, if it is a valid command, run it.
  void dispatch(GatewaySession& session, DisconnectCanceller& cod, const Responder& responder,
                const uint8_t* p, size_t n)
  {
    SessionReject rej{};
    // Real monotonic nanoseconds (rate-limit windows are wall-clock); the old
    // ++clock_ frame counter never advanced time -> permanent bans.
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    auto cmd = session.handle(p, n, nowNs, rej);
    if (cmd)
    {
      cod.track(*cmd);
      handler_(*cmd, responder, nowNs);
    }
    else if (rej != SessionReject::None && registry_ != nullptr)
    {
      registry_->send(session.account(),
                      OutboundEvent{OrderRejected{0, 0, toRejectReason(rej), session.account()}});
    }
  }

  GatewaySession::Decoder decoder_;
  uint64_t account_{0};
  Handler handler_;
  SocketAcceptor acceptor_;
  std::atomic<bool> cancelOnDisconnect_{false};
  SessionRegistry* registry_{nullptr};
  SessionRegistry::Encoder encoder_;
  SessionConfigHandler sessionCfg_;
  FixSessionHost* fixHost_{nullptr};
  GatewayCounters* counters_{nullptr};
  std::atomic<int64_t> idleTimeoutMs_{30'000};
};

}  // namespace flox::venue
