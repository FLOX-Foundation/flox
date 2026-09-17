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
 * POSIX sockets only, like the rest of flox/net.
 *
 * TLS is NOT here. The initiator talks to a SendFn and an onFrame(), so a TLS
 * channel is a drop-in replacement for this class rather than a change to the
 * session: see docs/venue/fix-initiator.md for why the encrypted variant lives
 * where OpenSSL is already a dependency rather than in the core.
 */
#pragma once

#include "flox/connector/fix/fix_initiator.h"
#include "flox/util/transport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <csignal>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace flox::fix
{

// A counterparty that vanishes mid-write must not take the process down.
// BSD and macOS suppress SIGPIPE per socket; Linux has no such option, so the
// signal is ignored process-wide once and EPIPE comes back to the writer.
inline void suppressSigpipe(int fd) noexcept
{
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
  static const bool ignored = []
  {
    ::signal(SIGPIPE, SIG_IGN);
    return true;
  }();
  (void)ignored;
#endif
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
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0)
    {
      return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (host == nullptr || host[0] == '\0')
    {
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    else if (::inet_pton(AF_INET, host, &a.sin_addr) != 1)
    {
      addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* res = nullptr;
      if (::getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr)
      {
        close();
        return false;
      }
      a.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
      ::freeaddrinfo(res);
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
    {
      close();
      return false;
    }
    suppressSigpipe(fd_);
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    timeval tv{recvTimeoutMs / 1000, (recvTimeoutMs % 1000) * 1000};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    reader_ = net::FrameReader{};
    return true;
  }

  bool send(const std::string& msg)
  {
    if (fd_ < 0)
    {
      return false;
    }
    return net::writeFrame(fd_, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  }

  Status read(std::string& out)
  {
    if (fd_ < 0)
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
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool connected() const noexcept { return fd_ >= 0; }
  int fd() const noexcept { return fd_; }

 private:
  int fd_{-1};
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
