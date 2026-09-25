/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Two edges of the socket layer that the portability suite does not cover.
//
// pollRead translates revents into a PollResult and reads POLLIN, POLLHUP and
// POLLERR out of it. POLLNVAL -- what poll reports for a descriptor that is
// closed or was never a descriptor -- is in none of those, so the call comes
// back with n == 1 and every field of the result false: not readable, not
// hung up, not an error, not a timeout. A caller looping "wait, then read"
// has nothing to break on and nothing to log, and the wait does not even
// block, so the loop runs at the speed of the syscall.
//
// The address family is the other one. openSocket hardcodes AF_INET, every
// address in the header is a sockaddr_in, and resolveIPv4 pins
// hints.ai_family to AF_INET. That is a real limit on where a perimeter can
// reach, and docs/reference/portable-sockets.md -- which does list what the
// layer covers and what it deliberately leaves out -- does not mention it.

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "flox/net/socket.h"

using namespace flox::net;

namespace
{

uint16_t listenLoopback(Handle& out)
{
  out = openSocket(Kind::Tcp);
  EXPECT_TRUE(valid(out));
  EXPECT_TRUE(setReuseAddr(out, true));
  EXPECT_TRUE(bindTo(out, "127.0.0.1", 0));
  EXPECT_TRUE(listenOn(out, 4));
  return boundPort(out);
}

bool decisive(const PollResult& r)
{
  return r.readable || r.timedOut || r.error || r.hangup || r.interrupted;
}

std::string readFile(const std::string& path)
{
  std::ifstream f(path);
  std::ostringstream all;
  all << f.rdbuf();
  return all.str();
}

}  // namespace

// The green control: pollRead's ordinary answers are right, so the failure
// below is about the one revent it does not read.
TEST(SocketPoll, AQuietSocketTimesOutAndAReadableOneIsSeen)
{
  Handle server = kInvalid;
  const uint16_t port = listenLoopback(server);

  Handle client = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(client));
  ASSERT_TRUE(connectTo(client, "127.0.0.1", port));

  const PollResult waiting = pollRead(server, 2000);
  ASSERT_TRUE(waiting.readable);
  Handle accepted = acceptOne(server);
  ASSERT_TRUE(valid(accepted));

  const PollResult idle = pollRead(accepted, 50);
  EXPECT_TRUE(idle.timedOut);
  EXPECT_TRUE(decisive(idle));

  const char b = 'x';
  ASSERT_EQ(sendNoSignal(client, &b, 1), 1);
  const PollResult ready = pollRead(accepted, 1000);
  EXPECT_TRUE(ready.readable);
  EXPECT_TRUE(decisive(ready));

  closeSocket(accepted);
  closeSocket(client);
  closeSocket(server);
}

TEST(SocketPoll, AClosedDescriptorIsReportedAsAnErrorAndNotAsNothing)
{
  Handle h = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(h));
  closeSocket(h);

  // The handle still holds the value the descriptor had. Asking to wait on it
  // is the shape of the bug: a reconnect that closed its socket while the
  // read loop still holds the number.
  const PollResult r = pollRead(h, 50);

  EXPECT_TRUE(decisive(r))
      << "pollRead on a closed descriptor returned readable=" << r.readable
      << " timedOut=" << r.timedOut << " error=" << r.error << " hangup=" << r.hangup
      << " interrupted=" << r.interrupted
      << ": every field false, so the caller learns nothing and retries";
  EXPECT_TRUE(r.error) << "a descriptor poll itself rejects is an error, not an idle socket";
  EXPECT_FALSE(r.readable);
}

// The consequence, measured rather than argued: the wait-then-read loop every
// caller of this layer writes does not terminate and does not block.
TEST(SocketPoll, AWaitThatCannotSucceedDoesNotSpin)
{
  Handle h = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(h));
  closeSocket(h);

  constexpr int kTimeoutMs = 50;
  constexpr int kCap = 200;

  const auto start = std::chrono::steady_clock::now();
  int turns = 0;
  PollResult r{};
  do
  {
    r = pollRead(h, kTimeoutMs);
    ++turns;
  } while (!decisive(r) && turns < kCap);

  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_LT(turns, kCap) << "the loop turned " << turns << " times in " << elapsedMs
                         << " ms without a single decisive answer: this is the 100% CPU "
                            "spin, with no diagnostic to log";
  EXPECT_TRUE(r.error);
}

// Either the layer carries IPv6, or the reference page says it does not.
// Today it does neither: the limit is real and undocumented, so a perimeter
// is designed against a header whose "portability layer" framing does not
// say which addresses it can reach.
//
// needs, for the "supported" ending -- any of:
//   enum class Family { IPv4, IPv6 };  Handle openSocket(Kind, Family);
//   plus sockaddr_storage-carrying parseAddress/bindTo/connectTo/resolve.
// The "documented" ending needs no interface, only the sentence.
TEST(SocketAddressFamily, TheLimitIsEitherLiftedOrWrittenDown)
{
  const std::string doc = readFile(std::string(FLOX_DOCS_DIR) + "/reference/portable-sockets.md");
  ASSERT_FALSE(doc.empty()) << "could not read docs/reference/portable-sockets.md";

  EXPECT_NE(doc.find("IPv6"), std::string::npos)
      << "the socket layer is IPv4-only -- openSocket passes AF_INET, every "
         "address is a sockaddr_in, resolveIPv4 pins hints.ai_family -- and "
         "the reference page never says so. Either add the family or say the "
         "layer does not carry one.";
}

// Whatever the answer to the previous test, an address the layer cannot carry
// has to be refused rather than quietly turned into some other address. This
// passes today and must keep passing.
TEST(SocketAddressFamily, AnAddressTheLayerCannotCarryIsRefused)
{
  ::sockaddr_in a{};
  EXPECT_FALSE(parseAddress("::1", a, 9000)) << "an IPv6 literal is not a sockaddr_in";
  EXPECT_FALSE(parseAddress("fe80::1", a, 9000));

  Handle h = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(h));
  EXPECT_FALSE(connectTo(h, "::1", 9000));
  EXPECT_FALSE(bindTo(h, "::1", 0));
  closeSocket(h);
}
