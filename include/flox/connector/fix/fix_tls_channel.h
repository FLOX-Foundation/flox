/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The FIX initiator's transport, encrypted.
 *
 * Drop-in for FixTcpClient: same connect/send/read/close, same
 * length-prefixed framing, same resumable read that returns Idle on a timeout
 * without losing the bytes it already has. FixInitiator talks to a SendFn and
 * an onFrame(), so the session layer does not know which of the two it is on.
 *
 * WHY IT IS HERE, BEHIND A FLAG
 *
 * The initiator lives in the core because a FIX client should not pay for a
 * matching engine. The core links no OpenSSL -- deliberately -- so this header
 * compiles only when FLOX_FIX_TLS is on, and the flag is off by default.
 * Nobody who does not ask for TLS acquires the dependency.
 *
 * The alternative placement considered for this channel was connectors/,
 * which already links OpenSSL. It also requires ZLIB and CURL and fetches
 * ixwebsocket and simdjson: three system libraries and two fetched projects to
 * get an encrypted FIX session. One optional dependency behind one flag is the
 * cheaper of the two, and that is the whole reason for the choice. (The venue
 * module is not involved either way: flox-venue links neither connectors nor
 * OpenSSL.)
 *
 * VERIFICATION IS ON
 *
 * The venue's own tls_gateway.h has a client context with SSL_VERIFY_NONE --
 * correct there, where it talks to a self-signed certificate in a test. It
 * would not be correct here. A counterparty's certificate is the only thing
 * between a FIX session and whoever happens to answer that port, so this
 * verifies by default, checks the hostname, and makes turning it off an
 * explicit, named act.
 */
#pragma once

#if !defined(FLOX_FIX_TLS)
#error "fix_tls_channel.h needs -DFLOX_FIX_TLS (configure with -DFLOX_FIX_TLS=ON)"
#endif

#include "flox/net/socket.h"
#include "flox/util/transport.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace flox::fix
{

class FixTlsChannel
{
 public:
  struct Options
  {
    // Off only on purpose. See the header comment.
    bool verifyPeer{true};
    std::string caFile;  // empty: the system trust store
    std::string caPath;  // empty: the system trust store
    // SNI and the name checked against the certificate. Empty means the host
    // passed to connect(), which is what a caller almost always means.
    std::string serverName;
  };

  enum class Status : uint8_t
  {
    Frame,   // `out` holds one complete FIX message
    Idle,    // the read timed out; partial bytes are kept for next time
    Closed,  // the peer closed, the handshake failed, or the socket did
  };

  ~FixTlsChannel() { close(); }
  FixTlsChannel() = default;
  FixTlsChannel(const FixTlsChannel&) = delete;
  FixTlsChannel& operator=(const FixTlsChannel&) = delete;

  // Two overloads rather than a defaulted Options: a nested type's own
  // defaults are not available as a default argument inside the class that
  // holds it.
  bool connect(const char* host, uint16_t port, int recvTimeoutMs = 200)
  {
    return connect(host, port, recvTimeoutMs, Options{});
  }

  bool connect(const char* host, uint16_t port, int recvTimeoutMs, Options opt)
  {
    close();
    const std::string hostName = (host == nullptr || host[0] == '\0') ? std::string{"127.0.0.1"}
                                                                      : std::string{host};
    fd_ = net::openSocket(net::Kind::Tcp);
    if (!net::valid(fd_))
    {
      return false;
    }
    ::sockaddr_in addr{};
    if ((!net::parseAddress(hostName, addr, port) && !net::resolveIPv4(hostName, addr, port)) ||
        !net::connectAddress(fd_, addr))
    {
      close();
      return false;
    }
    net::suppressSigPipe(fd_);
    net::setNoDelay(fd_, true);
    // The timeout is what turns a blocking read into the Idle the session layer
    // needs in order to run its timers.
    net::setReceiveTimeout(fd_, recvTimeoutMs);

    ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr)
    {
      close();
      return false;
    }
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    if (opt.verifyPeer)
    {
      SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
      const bool haveStore = !opt.caFile.empty() || !opt.caPath.empty();
      const bool loaded =
          haveStore ? SSL_CTX_load_verify_locations(ctx_, opt.caFile.empty() ? nullptr : opt.caFile.c_str(),
                                                    opt.caPath.empty() ? nullptr
                                                                       : opt.caPath.c_str()) == 1
                    : SSL_CTX_set_default_verify_paths(ctx_) == 1;
      if (!loaded)
      {
        // A verify that cannot load its trust store verifies nothing. Failing
        // to connect is the honest outcome; continuing would be verification
        // in name only.
        close();
        return false;
      }
    }
    else
    {
      SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    }

    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr || SSL_set_fd(ssl_, static_cast<int>(fd_)) != 1)
    {
      close();
      return false;
    }
    const std::string& name = opt.serverName.empty() ? hostName : opt.serverName;
    SSL_set_tlsext_host_name(ssl_, name.c_str());
    if (opt.verifyPeer)
    {
      // Without this the certificate is valid but not necessarily THIS
      // counterparty's -- any certificate the trust store accepts would do.
      SSL_set1_host(ssl_, name.c_str());
    }
    if (SSL_connect(ssl_) != 1)
    {
      // A handshake that failed leaves nothing readable: close() drops the
      // SSL* and the socket together, so no later read can pick bytes off a
      // half-established channel.
      close();
      return false;
    }
    reset();
    return true;
  }

  bool send(const std::string& msg)
  {
    if (ssl_ == nullptr)
    {
      return false;
    }
    uint8_t hdr[4];
    const auto n = static_cast<uint32_t>(msg.size());
    if (n > net::kMaxFrame)
    {
      return false;
    }
    hdr[0] = static_cast<uint8_t>(n >> 24);
    hdr[1] = static_cast<uint8_t>(n >> 16);
    hdr[2] = static_cast<uint8_t>(n >> 8);
    hdr[3] = static_cast<uint8_t>(n);
    return writeAll(hdr, sizeof hdr) &&
           writeAll(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  }

  // One FIX message per call, or Idle when the read timed out mid-frame. The
  // partial bytes stay, so a resumed read continues rather than restarting in
  // the middle of a message.
  Status read(std::string& out)
  {
    if (ssl_ == nullptr)
    {
      return Status::Closed;
    }
    if (!haveLen_)
    {
      const Status s = fill(hdr_, sizeof hdr_, hdrOff_);
      if (s != Status::Frame)
      {
        return s;
      }
      const uint32_t len = (static_cast<uint32_t>(hdr_[0]) << 24) |
                           (static_cast<uint32_t>(hdr_[1]) << 16) |
                           (static_cast<uint32_t>(hdr_[2]) << 8) | hdr_[3];
      if (len == 0 || len > net::kMaxFrame)
      {
        // A length this channel will not honour ends the session rather than
        // being skipped: the stream is no longer one this side can follow.
        close();
        return Status::Closed;
      }
      body_.assign(len, 0);
      bodyOff_ = 0;
      haveLen_ = true;
    }
    const Status s = fill(body_.data(), body_.size(), bodyOff_);
    if (s != Status::Frame)
    {
      return s;
    }
    out.assign(body_.begin(), body_.end());
    reset();
    return Status::Frame;
  }

  void close()
  {
    if (ssl_ != nullptr)
    {
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (ctx_ != nullptr)
    {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
    }
    net::closeSocket(fd_);
    fd_ = net::kInvalid;
    reset();
  }

  bool connected() const noexcept { return ssl_ != nullptr; }

 private:
  void reset() noexcept
  {
    hdrOff_ = 0;
    bodyOff_ = 0;
    haveLen_ = false;
    body_.clear();
  }

  bool writeAll(const uint8_t* p, size_t n)
  {
    size_t off = 0;
    while (off < n)
    {
      const int w = SSL_write(ssl_, p + off, static_cast<int>(n - off));
      if (w <= 0)
      {
        return false;
      }
      off += static_cast<size_t>(w);
    }
    return true;
  }

  // Read into `p` until `n` bytes have arrived, keeping the running offset so
  // a timeout can be resumed. Frame = complete, Idle = timed out with the
  // offset kept, Closed = the peer or the channel is gone.
  Status fill(uint8_t* p, size_t n, size_t& off)
  {
    while (off < n)
    {
      const int r = SSL_read(ssl_, p + off, static_cast<int>(n - off));
      if (r > 0)
      {
        off += static_cast<size_t>(r);
        continue;
      }
      const int err = SSL_get_error(ssl_, r);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
      {
        return Status::Idle;
      }
      if (err == SSL_ERROR_SYSCALL && net::wouldBlock(net::lastError()))
      {
        return Status::Idle;  // the receive timeout, which is what Idle means
      }
      return Status::Closed;
    }
    return Status::Frame;
  }

  net::Handle fd_{net::kInvalid};
  SSL_CTX* ctx_{nullptr};
  SSL* ssl_{nullptr};
  uint8_t hdr_[4]{};
  size_t hdrOff_{0};
  std::vector<uint8_t> body_;
  size_t bodyOff_{0};
  bool haveLen_{false};
};

}  // namespace flox::fix
