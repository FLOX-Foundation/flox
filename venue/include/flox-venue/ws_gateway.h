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
#include "flox/util/websocket.h"

#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace flox::venue
{

class WsGateway
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  using Handler = std::function<void(const InboundCommand&, const Responder&)>;

  explicit WsGateway(GatewaySession::Decoder decoder) : decoder_(std::move(decoder)) {}

  void setCancelOnDisconnect(bool on) noexcept { cancelOnDisconnect_.store(on); }

  int start(uint16_t port, Handler handler)
  {
    handler_ = std::move(handler);
    // connLoop runs as a per-connection thread body: an escaping exception would
    // reach std::terminate and kill the whole venue, so contain it to this one
    // connection (close the fd and let the thread exit cleanly).
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
    // 1. HTTP Upgrade handshake.
    std::string req;
    uint8_t tmp[2048];
    while (req.find("\r\n\r\n") == std::string::npos)
    {
      const ssize_t r = ::read(fd, tmp, sizeof tmp);
      if (r <= 0 || req.size() > (1u << 16))
      {
        ::close(fd);
        return;
      }
      req.append(reinterpret_cast<char*>(tmp), static_cast<size_t>(r));
    }
    const std::string resp = ws::handshakeResponse(req);
    if (resp.empty())
    {
      ::close(fd);
      return;
    }
    net::writeAll(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());

    // 2. Frame loop.
    GatewaySession session(0, decoder_);
    session.authenticate(true);
    const Responder responder = [fd](const uint8_t* p, size_t n)
    {
      const auto f = ws::buildFrame(ws::Opcode::Text, p, n);
      net::writeAll(fd, f.data(), f.size());
    };

    std::vector<uint8_t> buf;
    DisconnectCanceller cod(cancelOnDisconnect_.load());
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
        const ssize_t r = ::read(fd, tmp, sizeof tmp);
        if (r <= 0)
        {
          break;
        }
        buf.insert(buf.end(), tmp, tmp + r);
        continue;
      }
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));

      // Control frames are always final and may arrive mid-message.
      if (op == ws::Opcode::Close)
      {
        break;
      }
      if (op == ws::Opcode::Ping)
      {
        const auto pong = ws::buildFrame(ws::Opcode::Pong, payload.data(), payload.size());
        net::writeAll(fd, pong.data(), pong.size());
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
          dispatch(session, cod, responder, payload.data(), payload.size());
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
          dispatch(session, cod, responder, msg.data(), msg.size());
          assembling = false;
          msg.clear();
        }
        continue;
      }
      break;  // unknown opcode
    }
    cod.flush(handler_);
    ::close(fd);
  }

  // Decode one fully-assembled message and, if it is a valid command, run it.
  void dispatch(GatewaySession& session, DisconnectCanceller& cod, const Responder& responder,
                const uint8_t* p, size_t n)
  {
    SessionReject rej{};
    auto cmd = session.handle(p, n, ++clock_, rej);
    if (cmd)
    {
      cod.track(*cmd);
      handler_(*cmd, responder);
    }
  }

  GatewaySession::Decoder decoder_;
  Handler handler_;
  SocketAcceptor acceptor_;
  std::atomic<bool> cancelOnDisconnect_{false};
  std::atomic<int64_t> clock_{0};
};

}  // namespace flox::venue
