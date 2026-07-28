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
#include "flox-venue/messages.h"
#include "flox-venue/session.h"
#include "flox-venue/socket_acceptor.h"
#include "flox/util/transport.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <functional>
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

class TlsGateway
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  using Handler = std::function<void(const InboundCommand&, const Responder&)>;

  explicit TlsGateway(GatewaySession::Decoder decoder)
      : decoder_(std::move(decoder)), ctx_(tls::serverCtx())
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

  int start(uint16_t port, Handler handler)
  {
    handler_ = std::move(handler);
    // Contain any exception to this one connection -- connLoop is a thread body,
    // so an escape would reach std::terminate and take down the whole venue.
    return acceptor_.start(port, [this](int fd)
                           {
                             try { connLoop(fd); }
                             catch (...) { ::close(fd); } });
  }
  void stop() { acceptor_.stop(); }
  int port() const noexcept { return acceptor_.port(); }

 private:
  void connLoop(int fd)
  {
    SSL* ssl = SSL_new(ctx_);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) <= 0)
    {
      SSL_free(ssl);
      ::close(fd);
      return;
    }
    GatewaySession session(0, decoder_);
    session.authenticate(true);
    const Responder responder = [ssl](const uint8_t* p, size_t n)
    { tls::writeFrame(ssl, p, n); };
    std::vector<uint8_t> frame;
    DisconnectCanceller cod(cancelOnDisconnect_.load());
    while (acceptor_.running() && tls::readFrame(ssl, frame))
    {
      SessionReject rej{};
      auto cmd = session.handle(frame.data(), frame.size(), ++clock_, rej);
      if (cmd)
      {
        cod.track(*cmd);
        handler_(*cmd, responder);
      }
    }
    cod.flush(handler_);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(fd);
  }

  GatewaySession::Decoder decoder_;
  SSL_CTX* ctx_{nullptr};
  Handler handler_;
  SocketAcceptor acceptor_;
  std::atomic<bool> cancelOnDisconnect_{false};
  std::atomic<int64_t> clock_{0};
};

}  // namespace flox::venue
