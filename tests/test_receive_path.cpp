/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "flox/net/receive_path.h"

using flox::net::Datagram;
using flox::net::UdpSocketReceivePath;

namespace
{

int sendTo(uint16_t port, const std::string& payload)
{
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
  const auto n = ::sendto(fd, payload.data(), payload.size(), 0,
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ::close(fd);
  return static_cast<int>(n);
}

}  // namespace

TEST(UdpSocketReceivePath, BindsAndReportsTimestampMode)
{
  UdpSocketReceivePath::Config cfg;
  cfg.bindAddress = "127.0.0.1";
  cfg.port = 0;  // OS picks
  UdpSocketReceivePath rx(cfg);
  ASSERT_TRUE(rx.valid());
  EXPECT_GT(rx.localPort(), 0);
  // macOS delivers SOFTWARE (SO_TIMESTAMP); Linux SOFTWARE or HARDWARE
  // (hardware means requested-and-accepted; software stamps are always
  // requested alongside as the per-packet fallback).
  EXPECT_NE(rx.timestampMode(), flox::net::RxTimestampMode::NONE);
}

TEST(UdpSocketReceivePath, DeliversDatagramsWithTimestamps)
{
  UdpSocketReceivePath::Config cfg;
  cfg.bindAddress = "127.0.0.1";
  cfg.port = 0;
  UdpSocketReceivePath rx(cfg);
  ASSERT_TRUE(rx.valid());
  const auto port = rx.localPort();

  ASSERT_GT(sendTo(port, "alpha"), 0);
  ASSERT_GT(sendTo(port, "bravo"), 0);

  std::vector<std::string> got;
  std::vector<int64_t> stamps;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (got.size() < 2 && std::chrono::steady_clock::now() < deadline)
  {
    rx.poll(
        [&](const Datagram& d)
        {
          got.emplace_back(reinterpret_cast<const char*>(d.payload.data()),
                           d.payload.size());
          stamps.push_back(d.rxTimestampNs);
        },
        16);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], "alpha");
  EXPECT_EQ(got[1], "bravo");
  EXPECT_GT(stamps[0], 0);
  EXPECT_GT(stamps[1], 0);
}

TEST(UdpSocketReceivePath, PollRespectsMaxPacketsAndDrainsRest)
{
  UdpSocketReceivePath::Config cfg;
  cfg.bindAddress = "127.0.0.1";
  cfg.port = 0;
  UdpSocketReceivePath rx(cfg);
  ASSERT_TRUE(rx.valid());
  const auto port = rx.localPort();

  for (int i = 0; i < 5; ++i)
  {
    ASSERT_GT(sendTo(port, "pkt" + std::to_string(i)), 0);
  }
  // Give the loopback a moment to enqueue everything.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  int first = rx.poll([](const Datagram&) {}, 2);
  EXPECT_EQ(first, 2);

  int rest = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (rest < 3 && std::chrono::steady_clock::now() < deadline)
  {
    rest += rx.poll([](const Datagram&) {}, 16);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(rest, 3);
}

TEST(UdpSocketReceivePath, EmptyPollReturnsZero)
{
  UdpSocketReceivePath::Config cfg;
  cfg.bindAddress = "127.0.0.1";
  cfg.port = 0;
  UdpSocketReceivePath rx(cfg);
  ASSERT_TRUE(rx.valid());
  EXPECT_EQ(rx.poll([](const Datagram&) {}, 16), 0);
}
