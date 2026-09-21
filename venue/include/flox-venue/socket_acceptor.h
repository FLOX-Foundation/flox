/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox/net/socket.h"
#include "flox/util/concurrency/thread_body.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flox::venue
{

// A peer that vanishes mid-write must never take the process down. The three
// platforms answer this differently and the layer holds all three: a socket
// option on macOS, a send flag on Linux (see net::sendNoSignal, which every
// write here goes through), nothing at all on Windows. What is gone from this
// file is the process-wide signal(SIGPIPE, SIG_IGN) that used to stand in for
// the Linux case -- a library reaching into a process-wide signal disposition
// that the application may be relying on.
inline void suppressSigpipe(net::Handle fd) noexcept
{
  net::suppressSigPipe(fd);
}

class SocketAcceptor
{
 public:
  using OnConn = std::function<void(net::Handle fd)>;

  ~SocketAcceptor() { stop(); }

  // Listen on bindIp:port (0 = ephemeral). Returns the bound port, or -1.
  // bindIp nullptr/"" keeps the historical loopback-only default; an explicit
  // address ("0.0.0.0", a NIC address) exposes the listener beyond the host --
  // callers own the hardening contract for that (TLS termination / firewall in
  // front, see the per-server documentation). An unparseable address fails.
  int start(uint16_t port, OnConn onConn, const char* bindIp = nullptr)
  {
    onConn_ = std::move(onConn);
    const net::Handle fd = net::openSocket(net::Kind::Tcp);
    listenFd_.store(fd);
    if (!net::valid(fd))
    {
      return -1;
    }
    net::setReuseAddr(fd, true);
    const std::string ip = (bindIp == nullptr || bindIp[0] == '\0') ? std::string{"127.0.0.1"}
                                                                    : std::string{bindIp};
    if (!net::bindTo(fd, ip, port))
    {
      net::closeSocket(fd);
      listenFd_.store(net::kInvalid);
      return -1;
    }
    port_ = net::boundPort(fd);
    net::listenOn(fd, 16);
    running_.store(true);
    acceptThread_ = makeThread("venue.acceptor", [this]
                               { acceptLoop(); });
    return port_;
  }

  void stop()
  {
    if (!running_.exchange(false))
    {
      return;
    }
    // Take the descriptor out atomically: the accept thread reads it in its
    // accept call, so it must not observe a torn or dangling value.
    const net::Handle fd = listenFd_.exchange(net::kInvalid);
    if (net::valid(fd))
    {
      net::shutdownBoth(fd);
      net::closeSocket(fd);
    }
    if (acceptThread_.joinable())
    {
      acceptThread_.join();
    }
    // Shut every live connection down BEFORE joining: a connection thread
    // blocked in read()/SSL_read() on a silent peer would otherwise hold the
    // join forever (one black-holed client used to hang stop() indefinitely).
    // The fds in connFds_ are guaranteed open: a connection thread removes its
    // fd from the set under the lock before closing it.
    {
      std::lock_guard<std::mutex> lk(connsMutex_);
      // order: not observable -- every open fd is shut down, nothing is
      // emitted and no fd's treatment depends on another's
      for (net::Handle cfd : connFds_)
      {
        net::shutdownBoth(cfd);
      }
    }
    // The accept thread has been joined, so no new connection threads can be
    // appended; take them under the lock all the same and join outside it.
    std::vector<std::thread> conns;
    {
      std::lock_guard<std::mutex> lk(connsMutex_);
      conns.swap(conns_);
    }
    for (auto& t : conns)
    {
      if (t.joinable())
      {
        t.join();
      }
    }
  }

  int port() const noexcept { return port_; }
  bool running() const noexcept { return running_.load(); }

 private:
  void acceptLoop()
  {
    while (running_.load())
    {
      const net::Handle fd = net::acceptOne(listenFd_.load());
      if (!net::valid(fd))
      {
        if (!running_.load())
        {
          break;
        }
        continue;
      }
      net::setNoDelay(fd, true);
      suppressSigpipe(fd);
      std::lock_guard<std::mutex> lk(connsMutex_);
      connFds_.insert(fd);
      // The acceptor owns the fd lifecycle: the handler must NOT close it.
      // Deregistering under the lock before close keeps stop()'s shutdown
      // sweep away from a recycled descriptor number.
      conns_.push_back(makeThread("venue.acceptor.conn", [this, fd]
                                  {
                            // This lambda is a thread body, so an exception
                            // leaving the handler is std::terminate for the
                            // whole process -- one connection's parse error
                            // taking down every matching engine with it.
                            // Contain it here, where the descriptor is still
                            // deregistered and closed on the way out, so a
                            // handler cannot leak one by throwing either.
                            try
                            {
                              onConn_(fd);
                            }
                            catch (...)
                            {
                            }
                            {
                              std::lock_guard<std::mutex> lg(connsMutex_);
                              connFds_.erase(fd);
                            }
                            net::closeSocket(fd); }));
    }
  }

  OnConn onConn_;
  std::atomic<net::Handle> listenFd_{net::kInvalid};
  int port_{0};
  std::atomic<bool> running_{false};
  std::thread acceptThread_;
  std::mutex connsMutex_;
  std::vector<std::thread> conns_;
  std::unordered_set<net::Handle> connFds_;  // open connection fds (for stop()'s shutdown sweep)
};

}  // namespace flox::venue
