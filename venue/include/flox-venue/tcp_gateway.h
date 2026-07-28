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

  explicit TcpGateway(GatewaySession::Decoder decoder) : decoder_(std::move(decoder)) {}

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
    GatewaySession session(0, decoder_);
    session.authenticate(true);  // transport-level auth out of scope here
    const Responder responder = [fd](const uint8_t* p, size_t n)
    { net::writeFrame(fd, p, n); };
    std::vector<uint8_t> frame;
    DisconnectCanceller cod(cancelOnDisconnect_.load());
    while (acceptor_.running() && net::readFrame(fd, frame))
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
    ::close(fd);
  }

  GatewaySession::Decoder decoder_;
  Handler handler_;
  SocketAcceptor acceptor_;
  std::atomic<bool> cancelOnDisconnect_{false};
  std::atomic<int64_t> clock_{0};
};

}  // namespace flox::venue
