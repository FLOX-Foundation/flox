/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/control_api.h"
#include "flox-venue/socket_acceptor.h"
#include "flox/util/transport.h"

#include <unistd.h>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>

namespace flox::venue
{

class ControlServer
{
 public:
  explicit ControlServer(ControlApi& api) : api_(api) {}

  // Process every request line from `in`, writing one response line to `out`.
  void serve(std::istream& in, std::ostream& out)
  {
    std::string line;
    while (std::getline(in, line))
    {
      if (line.empty())
      {
        continue;
      }
      out << api_.handle(line) << "\n";
    }
  }

 private:
  ControlApi& api_;
};

// Network-exposed control plane: a loopback TCP admin server that serves the
// same line-delimited JSON request/response surface. A gRPC gateway or an MCP
// bridge fronts this in production; here it is directly reachable for admin
// tooling and tests.
class TcpControlServer
{
 public:
  explicit TcpControlServer(ControlApi& api) : api_(api) {}

  int start(uint16_t port)
  {
    return acceptor_.start(port, [this](int fd)
                           { connLoop(fd); });
  }
  void stop() { acceptor_.stop(); }
  int port() const noexcept { return acceptor_.port(); }

  // Upper bound on one request line. The framed transport caps a length prefix
  // at 16 MiB for the same reason (flox::net::kMaxFrame); this surface is
  // line-delimited, so the newline that never comes is the unbounded one. An
  // admin request is a short JSON object -- 1 MiB is already generous -- and a
  // client that sends more is not writing a request.
  static constexpr size_t kMaxRequestLine = 1u << 20;

 private:
  void connLoop(int fd)
  {
    std::string buf;
    uint8_t tmp[1024];
    while (acceptor_.running())
    {
      const ssize_t r = ::read(fd, tmp, sizeof tmp);
      if (r <= 0)
      {
        break;
      }
      buf.append(reinterpret_cast<char*>(tmp), static_cast<size_t>(r));
      size_t nl;
      while ((nl = buf.find('\n')) != std::string::npos)
      {
        const std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (line.empty())
        {
          continue;
        }
        const std::string resp = api_.handle(line) + "\n";
        net::writeAll(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
      }
      if (buf.size() > kMaxRequestLine)
      {
        static constexpr char kTooLong[] = "{\"ok\":false,\"error\":\"request_too_long\"}\n";
        net::writeAll(fd, reinterpret_cast<const uint8_t*>(kTooLong), sizeof kTooLong - 1);
        break;
      }
    }
    // The acceptor owns the descriptor and closes it once the handler returns.
    // Closing it here too hands the number back to the process while the
    // acceptor still holds it, and the second close then lands on whatever
    // another thread has since been given.
  }

  ControlApi& api_;
  SocketAcceptor acceptor_;
};

}  // namespace flox::venue
