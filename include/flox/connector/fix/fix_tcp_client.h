/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The client end of the framed TCP transport a flox venue listens on: one FIX
 * message per length-prefixed frame, the same shape TcpGateway serves. Reading
 * goes through flox::net::FrameReader, so a receive timeout in the middle of a
 * frame is resumable -- the session timers run and the reader comes back to the
 * bytes it already has, instead of resuming from the middle of a message.
 *
 * Sockets go through flox/net/socket.h, so this compiles and behaves the same
 * on every platform flox builds for.
 *
 * TLS is NOT here. The initiator talks to a SendFn and an onFrame(), so a TLS
 * channel is a drop-in replacement for this class rather than a change to the
 * session: see docs/venue/fix-initiator.md for why the encrypted variant lives
 * where OpenSSL is already a dependency rather than in the core.
 */
#pragma once

#include "flox/connector/fix/fix_initiator.h"
#include "flox/util/transport.h"

#include "flox/net/socket.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flox::fix
{

// A counterparty that vanishes mid-write must not take the process down. The
// layer holds the three platform answers; what is gone from here is the
// process-wide signal(SIGPIPE, SIG_IGN) this used to fall back to on Linux --
// a library has no business changing a signal disposition the application
// owns, and every write below goes through net::sendNoSignal, which carries
// the flag that makes it unnecessary.
inline void suppressSigpipe(net::Handle fd) noexcept
{
  net::suppressSigPipe(fd);
}

class FixTcpClient
{
 public:
  enum class Status : uint8_t
  {
    Frame,   // `out` holds one complete FIX message
    Idle,    // the read timed out; partial bytes are kept for next time
    Closed,  // the peer closed, or the socket failed
  };

  ~FixTcpClient() { close(); }

  // Connect to host:port. `recvTimeoutMs` is how long a read blocks before the
  // caller gets a turn to run the session timers; it bounds heartbeat latency,
  // so it wants to be well under the negotiated HeartBtInt.
  bool connect(const char* host, uint16_t port, int recvTimeoutMs = 200)
  {
    close();
    fd_ = net::openSocket(net::Kind::Tcp);
    if (!net::valid(fd_))
    {
      return false;
    }
    const std::string h = (host == nullptr || host[0] == '\0') ? std::string{"127.0.0.1"}
                                                               : std::string{host};
    // A dotted quad needs no lookup; a name does. Trying the cheap one first
    // keeps a resolver out of the path of every connect that does not need it.
    ::sockaddr_in a{};
    if (!net::parseAddress(h, a, port) && !net::resolveIPv4(h, a, port))
    {
      close();
      return false;
    }
    if (!net::connectAddress(fd_, a))
    {
      close();
      return false;
    }
    suppressSigpipe(fd_);
    net::setNoDelay(fd_, true);
    net::setReceiveTimeout(fd_, recvTimeoutMs);
    reader_ = net::FrameReader{};
    return true;
  }

  bool send(const std::string& msg)
  {
    if (!net::valid(fd_))
    {
      return false;
    }
    return net::writeFrame(fd_, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  }

  Status read(std::string& out)
  {
    if (!net::valid(fd_))
    {
      return Status::Closed;
    }
    std::vector<uint8_t> frame;
    switch (reader_.read(fd_, frame))
    {
      case net::FrameReader::Status::Frame:
        out.assign(frame.begin(), frame.end());
        return Status::Frame;
      case net::FrameReader::Status::Incomplete:
        return Status::Idle;
      case net::FrameReader::Status::Closed:
      default:
        return Status::Closed;
    }
  }

  void close()
  {
    net::closeSocket(fd_);
    fd_ = net::kInvalid;
  }

  bool connected() const noexcept { return net::valid(fd_); }
  net::Handle fd() const noexcept { return fd_; }

 private:
  net::Handle fd_{net::kInvalid};
  net::FrameReader reader_;
};

// Wires a FixInitiator onto a FixTcpClient: the initiator's SendFn writes
// frames, and poll() gives it one read attempt and one timer pass. Returns
// false once the session is over, for any of the three reasons a FIX session
// ends -- Logout exchanged, liveness lost, transport gone.
class FixTcpSession
{
 public:
  FixTcpSession(FixInitiator& initiator, FixTcpClient& client)
      : initiator_(initiator), client_(client)
  {
    initiator_.setSend([this](const std::string& m)
                       { return client_.send(m); });
  }

  bool connect(const char* host, uint16_t port, int64_t nowNs, int recvTimeoutMs = 200)
  {
    return client_.connect(host, port, recvTimeoutMs) && initiator_.connect(nowNs);
  }

  bool poll(int64_t nowNs)
  {
    std::string msg;
    for (;;)
    {
      const FixTcpClient::Status s = client_.read(msg);
      if (s == FixTcpClient::Status::Closed)
      {
        return false;
      }
      if (s == FixTcpClient::Status::Idle)
      {
        break;
      }
      if (initiator_.onFrame(msg, nowNs) == FixInitiator::Verdict::Disconnect)
      {
        return false;
      }
    }
    return initiator_.onTick(nowNs);
  }

  void close() { client_.close(); }

 private:
  FixInitiator& initiator_;
  FixTcpClient& client_;
};

}  // namespace flox::fix
