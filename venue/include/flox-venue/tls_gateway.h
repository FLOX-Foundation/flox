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

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace flox::venue
{
namespace tls
{

// Server context with a fresh in-memory self-signed P-256 certificate.
inline SSL_CTX* serverCtx()
{
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  EVP_PKEY* pkey = EVP_EC_gen("P-256");
  X509* x = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_getm_notBefore(x), 0);
  X509_gmtime_adj(X509_getm_notAfter(x), 31536000L);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
  X509_set_issuer_name(x, name);
  X509_sign(x, pkey, EVP_sha256());
  SSL_CTX_use_certificate(ctx, x);
  SSL_CTX_use_PrivateKey(ctx, pkey);
  X509_free(x);
  EVP_PKEY_free(pkey);
  return ctx;
}

inline SSL_CTX* clientCtx()
{
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);  // self-signed test cert
  return ctx;
}

inline bool writeAll(SSL* s, const uint8_t* p, size_t n)
{
  size_t o = 0;
  while (o < n)
  {
    const int w = SSL_write(s, p + o, static_cast<int>(n - o));
    if (w <= 0)
    {
      return false;
    }
    o += static_cast<size_t>(w);
  }
  return true;
}
inline bool readAll(SSL* s, uint8_t* p, size_t n)
{
  size_t o = 0;
  while (o < n)
  {
    const int r = SSL_read(s, p + o, static_cast<int>(n - o));
    if (r <= 0)
    {
      return false;
    }
    o += static_cast<size_t>(r);
  }
  return true;
}
inline bool writeFrame(SSL* s, const uint8_t* p, size_t n)
{
  const uint8_t h[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                        static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
  return writeAll(s, h, 4) && writeAll(s, p, n);
}
inline bool readFrame(SSL* s, std::vector<uint8_t>& out)
{
  uint8_t h[4];
  if (!readAll(s, h, 4))
  {
    return false;
  }
  const uint32_t len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
  // Reject a hostile length prefix before allocating: a 4-byte header must not
  // reserve up to 4 GiB over the TLS channel. Shares the core framing cap so the
  // two cannot drift apart.
  if (len > flox::net::kMaxFrame)
  {
    return false;
  }
  out.resize(len);
  return len == 0 || readAll(s, out.data(), len);
}

}  // namespace tls

// Delivery over TLS: OpenSSL forbids CONCURRENT SSL_read/SSL_write on one
// SSL*, not serialized use from two threads. Each connection carries a
// std::mutex over its SSL*: the read loop establishes readiness OUTSIDE the
// lock (::poll on the fd / SSL_has_pending) and takes it only for the actual
// SSL_read; the SessionWriter's write callback takes it for the SSL_write of
// one frame -- the two interleave, never overlap, and an idle reader cannot
// starve the writer by camping on the mutex. SSL_read keeps its own
// partial-record state across WANT_READ, so a frame split across polls is
// resumed, never lost. Without a registry the gateway stays in the embedded
// per-frame responder mode, exactly like TcpGateway.
class TlsGateway
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  using Handler = std::function<void(const InboundCommand&, const Responder&)>;

  // See TcpGateway: `account` binds each session so a client cannot spoof
  // another account's id. account == 0 is single-tenant-trusted mode.
  explicit TlsGateway(GatewaySession::Decoder decoder, uint64_t account = 0)
      : decoder_(std::move(decoder)), account_(account), ctx_(tls::serverCtx())
  {
  }
  ~TlsGateway()
  {
    stop();
    if (ctx_)
    {
      SSL_CTX_free(ctx_);
    }
  }

  void setCancelOnDisconnect(bool on) noexcept { cancelOnDisconnect_.store(on); }

  // Delivery mode (see TcpGateway::setDelivery): register every connection in
  // `registry` and deliver exec reports through per-session bounded queues.
  void setDelivery(SessionRegistry* registry, SessionRegistry::Encoder encoder)
  {
    registry_ = registry;
    encoder_ = std::move(encoder);
  }

  // Session-layer verbs (SBE resend/snapshot; see session_verbs.h). Only
  // consulted in delivery mode.
  void setSessionVerbs(SessionVerbHandler verbs) { verbs_ = std::move(verbs); }

  // Wire negotiation of per-session config (SBE SetSessionConfig -> COD; see
  // session_verbs.h makeSbeSessionConfigVerb). Fire-and-forget.
  void setSessionConfigVerb(SessionConfigHandler h) { sessionCfg_ = std::move(h); }

  // FIX session layer over TLS (fix_session.h; requires delivery mode). Same
  // wire shape as TcpGateway -- one FIX message per length-prefixed frame,
  // here inside the TLS stream: outbound goes through the existing
  // per-connection sslMu writer path (writeFrameLocked adds the length
  // prefix, so the FixConnection needs no wire wrap), inbound through the
  // poll-before-lock read loop. The FIX timers run on the read loop's poll
  // tick; FIX liveness (TestRequest death) replaces the plain idle timeout.
  void setFixSession(FixSessionHost* host) noexcept { fixHost_ = host; }

  void setCounters(GatewayCounters* counters) noexcept { counters_ = counters; }

  // Liveness: no inbound bytes for this long closes the session (COD fires).
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
  // Delivery mode polls SSL_read at this interval so the session writer can
  // take the SSL mutex between polls (and the shutdown sweep is noticed).
  static constexpr int64_t kPollMs = 20;

  enum class ReadResult : uint8_t
  {
    Frame,
    IdleTimeout,
    Closed,
  };

  // One length-prefixed frame over TLS. The mutex is taken only while there is
  // something to read: readiness is established OUTSIDE the lock (::poll on
  // the fd, or SSL_has_pending for records already buffered inside the SSL),
  // so an idle reader holds the mutex for microseconds per poll interval and
  // the writer thread is never starved by an unfair unlock/relock cycle.
  // SSL_read's internal record state and the running byte offset preserve a
  // frame split across polls; SO_RCVTIMEO bounds the lock hold when poll
  // reported a partial record. SSL_ERROR_ZERO_RETURN / SSL_ERROR_SYSCALL
  // (peer close, shutdown sweep) end the session.
  // `tick` (FIX mode): run on every idle poll interval; returning false ends
  // the session (FIX liveness lost -- reported as IdleTimeout so the caller's
  // counter accounting matches the plain-idle path).
  ReadResult readFrameLocked(int fd, SSL* ssl, std::mutex& mu, std::vector<uint8_t>& out,
                             int64_t idleMs, std::chrono::steady_clock::time_point& lastInbound,
                             const std::function<bool()>* tick = nullptr)
  {
    uint8_t h[4];
    uint32_t len = 0;
    bool haveLen = false;
    size_t got = 0;
    out.clear();
    while (acceptor_.running())
    {
      bool ready;
      {
        std::lock_guard<std::mutex> lk(mu);
        ready = SSL_has_pending(ssl) != 0;
      }
      if (!ready)
      {
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, static_cast<int>(kPollMs));
        if (pr < 0 && errno != EINTR)
        {
          return ReadResult::Closed;
        }
        ready = pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
      }
      if (!ready)
      {
        if (tick != nullptr)
        {
          if (!(*tick)())
          {
            return ReadResult::IdleTimeout;  // FIX liveness lost (TestRequest death)
          }
        }
        else if (idleMs > 0)
        {
          const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - lastInbound)
                                .count();
          if (idle >= idleMs)
          {
            return ReadResult::IdleTimeout;
          }
        }
        continue;
      }
      uint8_t* dst = haveLen ? out.data() + got : h + got;
      const size_t want = (haveLen ? len : sizeof h) - got;
      int r;
      int err = SSL_ERROR_NONE;
      {
        std::lock_guard<std::mutex> lk(mu);
        errno = 0;
        r = SSL_read(ssl, dst, static_cast<int>(want));
        if (r <= 0)
        {
          err = SSL_get_error(ssl, r);  // before any other call touches the SSL*
        }
      }
      if (r > 0)
      {
        lastInbound = std::chrono::steady_clock::now();
        got += static_cast<size_t>(r);
        if (!haveLen)
        {
          if (got < sizeof h)
          {
            continue;
          }
          len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
          if (len > flox::net::kMaxFrame)
          {
            return ReadResult::Closed;  // hostile length prefix (see tls::readFrame)
          }
          out.resize(len);
          haveLen = true;
          got = 0;
        }
        if (haveLen && got == len)
        {
          return ReadResult::Frame;
        }
        continue;
      }
      const bool retryable = err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
                             (err == SSL_ERROR_SYSCALL && wasRecvTimeout());
      if (!retryable)
      {
        return ReadResult::Closed;
      }
    }
    return ReadResult::Closed;
  }

  // One frame under one mutex acquisition: header and body leave as a single
  // SSL_write so the writer holds the SSL* for a bounded, contiguous burst.
  static bool writeFrameLocked(SSL* ssl, std::mutex& mu, const uint8_t* p, size_t n)
  {
    std::vector<uint8_t> buf(4 + n);
    buf[0] = static_cast<uint8_t>(n >> 24);
    buf[1] = static_cast<uint8_t>(n >> 16);
    buf[2] = static_cast<uint8_t>(n >> 8);
    buf[3] = static_cast<uint8_t>(n);
    std::memcpy(buf.data() + 4, p, n);
    std::lock_guard<std::mutex> lk(mu);
    return tls::writeAll(ssl, buf.data(), buf.size());
  }

  void connLoop(int fd)
  {
    const int64_t idleMs = idleTimeoutMs_.load();
    setRecvTimeoutMs(fd, idleMs);
    SSL* ssl = SSL_new(ctx_);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) <= 0)
    {
      SSL_free(ssl);
      return;  // acceptor owns the fd
    }
    GatewaySession session(account_, decoder_);
    session.authenticate(true);
    session.setCancelOnDisconnect(cancelOnDisconnect_.load());
    DisconnectCanceller cod(session.cancelOnDisconnect());

    std::mutex sslMu;  // serializes SSL_read (reader) vs SSL_write (writer thread)
    std::shared_ptr<SessionWriter> writer;
    if (registry_ != nullptr)
    {
      // Handshake done: switch to the short poll so a blocked SSL_read never
      // holds the mutex longer than one poll interval.
      setRecvTimeoutMs(fd, idleMs > 0 ? std::min<int64_t>(idleMs, kPollMs) : kPollMs);
      writer = registry_->attach(
          session.account(), encoder_,
          [ssl, &sslMu](const uint8_t* p, size_t n)
          { return writeFrameLocked(ssl, sslMu, p, n); },
          [fd]
          { ::shutdown(fd, SHUT_RDWR); },
          [&cod](const OutboundEvent& e)
          { cod.observe(e); });
    }
    // In delivery mode the responder feeds the same per-session queue as the
    // routed events (single writer owns the SSL*'s write side).
    const Responder responder =
        (writer != nullptr)
            ? Responder([w = writer.get()](const uint8_t* p, size_t n)
                        { w->enqueue(std::vector<uint8_t>(p, p + n)); })
            : Responder([ssl](const uint8_t* p, size_t n)
                        { tls::writeFrame(ssl, p, n); });

    // FIX mode (see setFixSession): the FixConnection gates every inbound
    // frame; its timers run on the read loop's poll tick via the tick hook.
    const bool fixMode = fixHost_ != nullptr && writer != nullptr;
    std::unique_ptr<FixConnection> fix;
    std::function<bool()> fixTick;
    if (fixMode)
    {
      fix = std::make_unique<FixConnection>(*fixHost_, *registry_, session.account(),
                                            wallClockNs());
      fix->setCodListener([&session, &cod](bool on)
                          {
                            session.setCancelOnDisconnect(on);
                            cod.setEnabled(on); });
      fixTick = [&fix]
      { return fix->onTick(wallClockNs()); };
    }

    auto lastInbound = std::chrono::steady_clock::now();
    std::vector<uint8_t> frame;
    while (acceptor_.running())
    {
      const ReadResult rr = readFrameLocked(fd, ssl, sslMu, frame, idleMs, lastInbound,
                                            fixMode ? &fixTick : nullptr);
      if (rr != ReadResult::Frame)
      {
        if (rr == ReadResult::IdleTimeout && counters_ != nullptr)
        {
          counters_->idleDisconnects.fetch_add(1, std::memory_order_relaxed);
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
        // Verdict::App: fall through to the decoder / admission path.
      }
      if (sessionCfg_)
      {
        if (const auto u = sessionCfg_(frame.data(), frame.size()))
        {
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
      // Real monotonic nanoseconds (rate-limit windows are wall-clock); the old
      // ++clock_ frame counter never advanced time -> permanent bans.
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
        // A rejected frame answers with a sequenced exec-report reject (id 0)
        // instead of the old silence -- same policy as TcpGateway.
        registry_->send(session.account(),
                        OutboundEvent{OrderRejected{0, 0, toRejectReason(rej),
                                                    session.account()}});
      }
    }
    if (writer != nullptr)
    {
      // Order matters: detach (no new frames are routed here), stop (drain and
      // JOIN the writer thread -- its last SSL_write finishes) and only then
      // SSL_shutdown/SSL_free below. The writer never sees a freed SSL*.
      registry_->detach(session.account(), writer);
      writer->stop();
    }
    cod.flush(handler_);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::shutdown(fd, SHUT_RDWR);  // acceptor owns the close
  }

  GatewaySession::Decoder decoder_;
  uint64_t account_{0};
  SSL_CTX* ctx_{nullptr};
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
