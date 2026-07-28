/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/socket_acceptor.h"
#include "flox/util/transport.h"

#include <unistd.h>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace flox::venue
{

class MetricsServer
{
 public:
  using Snapshot = std::function<std::string()>;  // /metrics body
  using Ready = std::function<bool()>;

  explicit MetricsServer(Snapshot snap, Ready ready = []
                                        { return true; }) : snap_(std::move(snap)), ready_(std::move(ready))
  {
  }

  int start(uint16_t port)
  {
    return acceptor_.start(port, [this](int fd)
                           { connLoop(fd); });
  }
  void stop() { acceptor_.stop(); }
  int port() const noexcept { return acceptor_.port(); }

 private:
  static void respond(int fd, const char* status, const char* ctype, const std::string& body)
  {
    std::string r = "HTTP/1.1 ";
    r += status;
    r += "\r\nContent-Type: ";
    r += ctype;
    r += "\r\nContent-Length: ";
    r += std::to_string(body.size());
    r += "\r\nConnection: close\r\n\r\n";
    r += body;
    net::writeAll(fd, reinterpret_cast<const uint8_t*>(r.data()), r.size());
  }

  void connLoop(int fd)
  {
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

    // Parse "GET <path> HTTP/1.1".
    const size_t sp1 = req.find(' ');
    const size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
    std::string path;
    if (sp1 != std::string::npos && sp2 != std::string::npos)
    {
      path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    if (path == "/metrics")
    {
      respond(fd, "200 OK", "text/plain; version=0.0.4", snap_());
    }
    else if (path == "/healthz")
    {
      respond(fd, "200 OK", "text/plain", "ok\n");
    }
    else if (path == "/readyz")
    {
      if (ready_())
      {
        respond(fd, "200 OK", "text/plain", "ready\n");
      }
      else
      {
        respond(fd, "503 Service Unavailable", "text/plain", "not ready\n");
      }
    }
    else
    {
      respond(fd, "404 Not Found", "text/plain", "not found\n");
    }
    ::close(fd);
  }

  Snapshot snap_;
  Ready ready_;
  SocketAcceptor acceptor_;
};

}  // namespace flox::venue
