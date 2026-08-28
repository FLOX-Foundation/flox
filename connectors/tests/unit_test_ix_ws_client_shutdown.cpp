/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline shutdown test: IxWebSocketClient stop() racing its own run() loop.
 *
 * run() calls ix::WebSocket::stop() from its reconnect loop; stop() used to
 * call it too, from whichever thread asked for shutdown. With a LIVE ix worker
 * both threads then join it, the loser gets EINVAL and ix throws -- out of the
 * run() thread body, which is std::terminate before anything downstream (tape
 * writers, journals) gets to flush. stop() also read _ws unsynchronized while
 * run() replaced it under _sendMutex on each reconnect attempt.
 *
 * Hitting the race takes a join that is SLOW. Two versions of this test
 * failed to kill the unfixed client and taught the shape of the window:
 * against a closed port the worker is dead before stop() arrives, and against
 * a listener that accepts but never finishes the handshake, close() collapses
 * the worker instantly -- both joins are microseconds, and the two stops
 * never overlap. What made the production crash reproduce 3/3 is an
 * ESTABLISHED session whose peer does not answer the close frame: ix then
 * waits kClosingMaximumWaitingDelayInMs (300ms) before giving up, the first
 * join blocks for that long, and the run() thread -- polling every 100ms --
 * always arrives inside it. So the listener here completes a real websocket
 * handshake (Sec-WebSocket-Accept via ix's own header-only keygen) and then
 * goes mute.
 *
 * A process cannot catch its own std::terminate, so surviving the loop IS the
 * assertion. Everything stays on 127.0.0.1.
 */

#include "flox-connectors/net/ix_websocket_client.h"

// ix's own header-only Sec-WebSocket-Accept generator; using it keeps this
// test free of a hand-rolled SHA1.
#include <ixwebsocket/IXWebSocketHandshakeKeyGen.h>

#include <flox/log/abstract_logger.h>

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct NullLogger final : public flox::ILogger
{
  void info(std::string_view) override {}
  void warn(std::string_view) override {}
  void error(std::string_view) override {}
};

// Completes the websocket handshake and then goes mute: reads nothing more,
// answers nothing -- including the close frame, which is what stretches the
// client's teardown into the 300ms window the race needs.
class MuteListener
{
 public:
  MuteListener()
  {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    ::listen(fd_, 16);
    th_ = std::thread(
        [this]
        {
          while (running_.load())
          {
            int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0)
            {
              break;
            }
            answerHandshake(c);
            std::lock_guard<std::mutex> lk(m_);
            clients_.push_back(c);  // held open; close frames go unanswered
          }
        });
  }

  ~MuteListener()
  {
    running_.store(false);
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    if (th_.joinable())
    {
      th_.join();
    }
    std::lock_guard<std::mutex> lk(m_);
    for (int c : clients_)
    {
      ::close(c);
    }
  }

  uint16_t port() const { return port_; }

 private:
  static void answerHandshake(int c)
  {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos)
    {
      const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
      if (n <= 0)
      {
        return;
      }
      req.append(buf, static_cast<size_t>(n));
    }
    const std::string marker = "Sec-WebSocket-Key: ";
    const size_t at = req.find(marker);
    if (at == std::string::npos)
    {
      return;
    }
    const size_t end = req.find("\r\n", at);
    const std::string key = req.substr(at + marker.size(), end - (at + marker.size()));

    char accept[29] = {};
    WebSocketHandshakeKeyGen::generate(key, accept);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    resp += accept;
    resp += "\r\n\r\n";
    ::send(c, resp.data(), resp.size(), 0);
  }

 private:
  int fd_{-1};
  uint16_t port_{0};
  std::atomic<bool> running_{true};
  std::thread th_;
  std::mutex m_;
  std::vector<int> clients_;
};

}  // namespace

TEST(IxWebSocketClientShutdown, StopFromAnotherThreadWhileReconnecting)
{
  NullLogger logger;
  MuteListener listener;
  const std::string url = "ws://127.0.0.1:" + std::to_string(listener.port());

  // Seeded, not random: a failure must replay.
  std::mt19937 rng(0x51CE);
  std::uniform_int_distribution<int> jitterUs(0, 30'000);

  for (int i = 0; i < 25; ++i)
  {
    auto client = std::make_unique<flox::IxWebSocketClient>(url, "", /*reconnectDelayMs*/ 1,
                                                            &logger, /*pingIntervalSec*/ 0);
    client->start();
    // Give the handshake time to complete so the session is established, then
    // land stop() at a varying phase. From here the racy client's two threads
    // both join one live worker whose exit is pinned open for 300ms by the
    // unanswered close frame; the run() thread's 100ms poll always lands
    // inside that, and its loss is the fatal one.
    std::this_thread::sleep_for(std::chrono::microseconds(30'000 + jitterUs(rng)));
    client->stop();
    client.reset();  // destructor joins the run() thread
  }

  // Reaching this line is the assertion: the old code terminated the process
  // out of the run() thread before the loop finished.
  SUCCEED();
}
