/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Who owns a connection, and what one connection is allowed to take down.
//
// The acceptor owns the descriptor: it hands a number to a handler, waits for
// the handler to return, and closes it. A handler that also closes it returns a
// descriptor number to the pool that the acceptor then closes a second time --
// by which point another thread may already have been given it. This test
// counts closes per descriptor directly, by interposing on close(2), so the
// property is asserted rather than inferred from a race that may or may not
// fire.
//
// The second property is the handler's exception. It runs as a thread body, so
// an escape is std::terminate for the whole venue -- one malformed admin
// request taking the matching engines with it.

#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/control_server.h"
#include "flox-venue/metrics_server.h"
#include "flox-venue/socket_acceptor.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

using namespace flox;
using namespace flox::venue;

namespace
{

std::mutex g_closeMutex;
std::unordered_map<int, int> g_closeCounts;
std::atomic<bool> g_counting{false};

void noteClose(int fd)
{
  if (!g_counting.load(std::memory_order_relaxed))
  {
    return;
  }
  std::lock_guard<std::mutex> lk(g_closeMutex);
  ++g_closeCounts[fd];
}

int worstCount()
{
  std::lock_guard<std::mutex> lk(g_closeMutex);
  int worst = 0;
  for (const auto& [fd, n] : g_closeCounts)
  {
    worst = n > worst ? n : worst;
  }
  return worst;
}

int totalCount()
{
  std::lock_guard<std::mutex> lk(g_closeMutex);
  int total = 0;
  for (const auto& [fd, n] : g_closeCounts)
  {
    total += n;
  }
  return total;
}

int connectLoopback(int port)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return -1;
  }
  suppressSigpipe(fd);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
  {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

// Interpose on close(2) for this test binary. Both closes under test live in
// headers, so they compile into this translation unit and bind here; the real
// close is reached through the next object in the search order, so every other
// descriptor in the process behaves normally.
extern "C" int close(int fd)
{
  noteClose(fd);
  using CloseFn = int (*)(int);
  static CloseFn real = reinterpret_cast<CloseFn>(::dlsym(RTLD_NEXT, "close"));
  return real != nullptr ? real(fd) : -1;
}

TEST(VenueConnectionLifecycle, ControlServerClosesEachDescriptorOnce)
{
  InstrumentRegistry reg;
  ControlApi api(reg);
  TcpControlServer srv(api);
  const int port = srv.start(0);
  ASSERT_GT(port, 0);

  const int client = connectLoopback(port);
  ASSERT_GE(client, 0);

  const std::string req = R"({"method":"list"})"
                          "\n";
  ASSERT_EQ(::send(client, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
  char buf[256];
  ASSERT_GT(::recv(client, buf, sizeof buf, 0), 0);

  // From here on, every close is counted. Dropping the client ends the server's
  // read loop, which is where the handler used to close a descriptor the
  // acceptor closes again a moment later.
  {
    std::lock_guard<std::mutex> lk(g_closeMutex);
    g_closeCounts.clear();
  }
  g_counting.store(true);
  ::close(client);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  srv.stop();  // joins the connection thread
  g_counting.store(false);

  // Guard against a vacuous pass: if the interposition below is not in the call
  // path -- a sanitizer runtime interceptor ahead of it, a build that inlined
  // the syscall -- nothing is counted and every assertion holds trivially.
  ASSERT_GE(totalCount(), 1) << "no close was observed at all; the interposition did not take, "
                                "so this test proves nothing";
  EXPECT_LE(worstCount(), 1) << "a descriptor closed twice is a descriptor another thread may "
                                "already hold";
}

// The metrics endpoint runs on the same acceptor and had the same habit, on
// both of its exits. Under load it is the one most likely to be hit: a scrape
// every few seconds, one connection each, against a process opening journal
// segments on other threads.
TEST(VenueConnectionLifecycle, MetricsServerClosesEachDescriptorOnce)
{
  MetricsServer srv([]
                    { return std::string("flox_up 1\n"); });
  const int port = srv.start(0);
  ASSERT_GT(port, 0);

  const int client = connectLoopback(port);
  ASSERT_GE(client, 0);

  {
    std::lock_guard<std::mutex> lk(g_closeMutex);
    g_closeCounts.clear();
  }
  g_counting.store(true);

  const std::string req = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";
  ASSERT_EQ(::send(client, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
  char buf[512];
  std::string answer;
  ssize_t r = 0;
  while ((r = ::recv(client, buf, sizeof buf, 0)) > 0)
  {
    answer.append(buf, static_cast<size_t>(r));
  }
  EXPECT_NE(answer.find("200 OK"), std::string::npos) << answer;

  ::close(client);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  srv.stop();
  g_counting.store(false);

  ASSERT_GE(totalCount(), 1) << "no close was observed at all; the interposition did not take, "
                                "so this test proves nothing";
  EXPECT_LE(worstCount(), 1) << "a descriptor closed twice is a descriptor another thread may "
                                "already hold";
}

TEST(VenueConnectionLifecycle, HandlerExceptionKillsOnlyItsConnection)
{
  SocketAcceptor acc;
  std::atomic<int> entered{0};
  const int port = acc.start(0, [&entered](int)
                             {
                               entered.fetch_add(1);
                               throw std::runtime_error("handler failed"); });
  ASSERT_GT(port, 0);

  const int client = connectLoopback(port);
  ASSERT_GE(client, 0);
  for (int i = 0; i < 200 && entered.load() == 0; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(entered.load(), 1);

  ::close(client);
  acc.stop();

  // A second connection is still served: the acceptor survived the first.
  std::atomic<int> served{0};
  SocketAcceptor acc2;
  const int port2 = acc2.start(0, [&served](int)
                               { served.fetch_add(1); });
  ASSERT_GT(port2, 0);
  const int client2 = connectLoopback(port2);
  ASSERT_GE(client2, 0);
  for (int i = 0; i < 200 && served.load() == 0; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(served.load(), 1);
  ::close(client2);
  acc2.stop();
}
