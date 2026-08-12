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

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace flox::venue
{

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
    GatewaySession session(account_, decoder_);
    session.authenticate(true);  // transport-level auth out of scope here
    const Responder responder = [fd](const uint8_t* p, size_t n)
    { net::writeFrame(fd, p, n); };
    std::vector<uint8_t> frame;
    DisconnectCanceller cod(cancelOnDisconnect_.load());
    while (acceptor_.running() && net::readFrame(fd, frame))
    {
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
    }
    cod.flush(handler_);
    ::close(fd);
  }

  GatewaySession::Decoder decoder_;
  uint64_t account_{0};
  Handler handler_;
  SocketAcceptor acceptor_;
  std::atomic<bool> cancelOnDisconnect_{false};
};

}  // namespace flox::venue
