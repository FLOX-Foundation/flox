/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flox::venue
{

// A peer that vanishes mid-write must never take the process down. BSD/macOS
// suppress SIGPIPE per-socket (SO_NOSIGPIPE); Linux has no such option and
// SSL_write cannot pass MSG_NOSIGNAL, so we ignore the signal process-wide once.
// EPIPE is then returned to the writer, which closes the session cleanly.
inline void suppressSigpipe(int fd) noexcept
{
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
  static const bool ignored = []
  { ::signal(SIGPIPE, SIG_IGN); return true; }();
  (void)ignored;
#endif
}

class SocketAcceptor
{
 public:
  using OnConn = std::function<void(int fd)>;

  ~SocketAcceptor() { stop(); }

  // Listen on bindIp:port (0 = ephemeral). Returns the bound port, or -1.
  // bindIp nullptr/"" keeps the historical loopback-only default; an explicit
  // address ("0.0.0.0", a NIC address) exposes the listener beyond the host --
  // callers own the hardening contract for that (TLS termination / firewall in
  // front, see the per-server documentation). An unparseable address fails.
  int start(uint16_t port, OnConn onConn, const char* bindIp = nullptr)
  {
    onConn_ = std::move(onConn);
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    listenFd_.store(fd);
    if (fd < 0)
    {
      return -1;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bindIp != nullptr && bindIp[0] != '\0' && ::inet_pton(AF_INET, bindIp, &a.sin_addr) != 1)
    {
      ::close(fd);
      listenFd_.store(-1);
      return -1;
    }
    a.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
    {
      ::close(fd);
      listenFd_.store(-1);
      return -1;
    }
    socklen_t sl = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &sl);
    port_ = ntohs(a.sin_port);
    ::listen(fd, 16);
    running_.store(true);
    acceptThread_ = std::thread([this]
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
    // ::accept() call, so it must not observe a torn or dangling value.
    const int fd = listenFd_.exchange(-1);
    if (fd >= 0)
    {
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
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
      for (int cfd : connFds_)
      {
        ::shutdown(cfd, SHUT_RDWR);
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
      const int fd = ::accept(listenFd_.load(), nullptr, nullptr);
      if (fd < 0)
      {
        if (!running_.load())
        {
          break;
        }
        continue;
      }
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      suppressSigpipe(fd);
      std::lock_guard<std::mutex> lk(connsMutex_);
      connFds_.insert(fd);
      // The acceptor owns the fd lifecycle: the handler must NOT close it.
      // Deregistering under the lock before close keeps stop()'s shutdown
      // sweep away from a recycled descriptor number.
      conns_.emplace_back([this, fd]
                          {
                            onConn_(fd);
                            {
                              std::lock_guard<std::mutex> lg(connsMutex_);
                              connFds_.erase(fd);
                            }
                            ::close(fd); });
    }
  }

  OnConn onConn_;
  std::atomic<int> listenFd_{-1};
  int port_{0};
  std::atomic<bool> running_{false};
  std::thread acceptThread_;
  std::mutex connsMutex_;
  std::vector<std::thread> conns_;
  std::unordered_set<int> connFds_;  // open connection fds (for stop()'s shutdown sweep)
};

}  // namespace flox::venue
