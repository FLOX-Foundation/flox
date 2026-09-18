// The socket layer, exercised rather than declared. A portability header
// that nothing calls proves only that it compiles.

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "flox/net/socket.h"

using namespace flox::net;

namespace
{

// A listening TCP socket on a free loopback port. Returns the port.
uint16_t listenLoopback(Handle& out)
{
  out = openSocket(Kind::Tcp);
  EXPECT_TRUE(valid(out));
  EXPECT_TRUE(setReuseAddr(out, true));
  ::sockaddr_in a{};
  EXPECT_TRUE(parseAddress("127.0.0.1", a, 0));
  EXPECT_EQ(::bind(out, reinterpret_cast<::sockaddr*>(&a), sizeof a), 0);
  EXPECT_EQ(::listen(out, 4), 0);
  ::sockaddr_in bound{};
#if defined(_WIN32)
  int len = sizeof bound;
#else
  ::socklen_t len = sizeof bound;
#endif
  EXPECT_EQ(::getsockname(out, reinterpret_cast<::sockaddr*>(&bound), &len), 0);
  return ntohs(bound.sin_port);
}

Handle connectLoopback(uint16_t port)
{
  Handle c = openSocket(Kind::Tcp);
  EXPECT_TRUE(valid(c));
  ::sockaddr_in a{};
  EXPECT_TRUE(parseAddress("127.0.0.1", a, port));
  EXPECT_EQ(::connect(c, reinterpret_cast<::sockaddr*>(&a), sizeof a), 0);
  return c;
}

}  // namespace

TEST(Socket, StartupIsIdempotentAndOpenGivesAValidHandle)
{
  EXPECT_TRUE(ensureStarted());
  EXPECT_TRUE(ensureStarted()) << "called again, still fine";
  Handle t = openSocket(Kind::Tcp);
  Handle u = openSocket(Kind::Udp);
  EXPECT_TRUE(valid(t));
  EXPECT_TRUE(valid(u));
  closeSocket(t);
  closeSocket(u);
  EXPECT_FALSE(valid(kInvalid));
  closeSocket(kInvalid);  // must not crash
}

TEST(Socket, OptionsApply)
{
  Handle h = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(h));
  EXPECT_TRUE(setReuseAddr(h, true));
  EXPECT_TRUE(setNoDelay(h, true));
  EXPECT_TRUE(setSendBufferBytes(h, 64 * 1024));
  EXPECT_TRUE(suppressSigPipe(h));
  EXPECT_TRUE(setNonBlocking(h, true));
  EXPECT_TRUE(setNonBlocking(h, false));
  closeSocket(h);

  Handle u = openSocket(Kind::Udp);
  ASSERT_TRUE(valid(u));
  EXPECT_TRUE(setMulticastTtl(u, 1));
  closeSocket(u);
}

// The dangerous one. A timeout set in milliseconds must actually elapse in
// milliseconds: the whole point is that the argument's shape differs and the
// call succeeds either way.
TEST(Socket, ReceiveTimeoutElapsesInTheMillisecondsItWasGiven)
{
  Handle server = kInvalid;
  const uint16_t port = listenLoopback(server);
  Handle client = connectLoopback(port);
  Handle accepted = ::accept(server, nullptr, nullptr);
  ASSERT_TRUE(valid(accepted));

  ASSERT_TRUE(setReceiveTimeout(accepted, 150));
  char buf[16];
  const auto t0 = std::chrono::steady_clock::now();
  const long n = receive(accepted, buf, sizeof buf);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  EXPECT_LE(n, 0) << "nothing was sent; the call must return empty-handed";
  EXPECT_TRUE(wouldBlock(lastError())) << "and say it was a timeout, not a broken socket";
  EXPECT_GE(elapsed, 120) << "a 150 ms timeout that returns at once was set wrong";
  EXPECT_LT(elapsed, 2000) << "a 150 ms timeout that waits seconds was set wrong the other way";

  closeSocket(accepted);
  closeSocket(client);
  closeSocket(server);
}

TEST(Socket, NonBlockingReceiveReturnsAtOnceAndSaysWhy)
{
  Handle server = kInvalid;
  const uint16_t port = listenLoopback(server);
  Handle client = connectLoopback(port);
  Handle accepted = ::accept(server, nullptr, nullptr);
  ASSERT_TRUE(valid(accepted));
  ASSERT_TRUE(setNonBlocking(accepted, true));

  char buf[16];
  const auto t0 = std::chrono::steady_clock::now();
  const long n = receive(accepted, buf, sizeof buf);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  EXPECT_LE(n, 0);
  EXPECT_TRUE(wouldBlock(lastError()));
  EXPECT_LT(elapsed, 50) << "non-blocking means now";

  closeSocket(accepted);
  closeSocket(client);
  closeSocket(server);
}

TEST(Socket, SendAndReceiveRoundTripAndPeekDoesNotConsume)
{
  Handle server = kInvalid;
  const uint16_t port = listenLoopback(server);
  Handle client = connectLoopback(port);
  Handle accepted = ::accept(server, nullptr, nullptr);
  ASSERT_TRUE(valid(accepted));
  ASSERT_TRUE(suppressSigPipe(client));

  const std::string msg = "hello";
  ASSERT_EQ(sendNoSignal(client, msg.data(), msg.size()), static_cast<long>(msg.size()));

  char buf[16] = {};
  ASSERT_EQ(receive(accepted, buf, sizeof buf, /*peek=*/true), 5);
  EXPECT_EQ(std::string(buf, 5), "hello");
  std::memset(buf, 0, sizeof buf);
  ASSERT_EQ(receive(accepted, buf, sizeof buf), 5) << "peek left the bytes where they were";
  EXPECT_EQ(std::string(buf, 5), "hello");

  closeSocket(accepted);
  closeSocket(client);
  closeSocket(server);
}

TEST(Socket, SendingToAClosedPeerFailsAndDoesNotKillTheProcess)
{
  // On macOS this is what SO_NOSIGPIPE buys; on Linux, MSG_NOSIGNAL inside
  // sendNoSignal. Without either, this test would not fail -- the process
  // would die.
  Handle server = kInvalid;
  const uint16_t port = listenLoopback(server);
  Handle client = connectLoopback(port);
  Handle accepted = ::accept(server, nullptr, nullptr);
  ASSERT_TRUE(valid(accepted));
  ASSERT_TRUE(suppressSigPipe(client));

  shutdownBoth(accepted);
  closeSocket(accepted);
  closeSocket(server);

  const std::string msg(4096, 'x');
  long total = 0;
  for (int i = 0; i < 64; ++i)
  {
    const long n = sendNoSignal(client, msg.data(), msg.size());
    if (n < 0)
    {
      break;
    }
    total += n;
  }
  SUCCEED() << "still running after writing " << total << " bytes to a dead peer";
  closeSocket(client);
}

TEST(Socket, PollSeesReadableAndTimesOut)
{
  Handle server = kInvalid;
  const uint16_t port = listenLoopback(server);
  Handle client = connectLoopback(port);
  Handle accepted = ::accept(server, nullptr, nullptr);
  ASSERT_TRUE(valid(accepted));

  auto idle = pollRead(accepted, 50);
  EXPECT_TRUE(idle.timedOut);
  EXPECT_FALSE(idle.readable);

  const char b = 'x';
  ASSERT_EQ(sendNoSignal(client, &b, 1), 1);
  auto ready = pollRead(accepted, 1000);
  EXPECT_TRUE(ready.readable);
  EXPECT_FALSE(ready.timedOut);

  closeSocket(accepted);
  closeSocket(client);
  closeSocket(server);
}

TEST(Socket, AddressParsingAcceptsWhatItShouldAndRefusesWhatItShouldNot)
{
  ::sockaddr_in a{};
  EXPECT_TRUE(parseAddress("127.0.0.1", a, 9000));
  EXPECT_EQ(ntohs(a.sin_port), 9000);
  EXPECT_TRUE(parseAddress("", a, 1)) << "empty means any interface";
  EXPECT_EQ(a.sin_addr.s_addr, htonl(INADDR_ANY));
  EXPECT_FALSE(parseAddress("not-an-address", a, 1));
  EXPECT_FALSE(parseAddress("999.1.1.1", a, 1));
}

TEST(Socket, MulticastJoinAcceptsAGroupAndRefusesAUnicastAddress)
{
  Handle u = openSocket(Kind::Udp);
  ASSERT_TRUE(valid(u));
  ASSERT_TRUE(setReuseAddr(u, true));
  ::sockaddr_in a{};
  ASSERT_TRUE(parseAddress("", a, 0));
  ASSERT_EQ(::bind(u, reinterpret_cast<::sockaddr*>(&a), sizeof a), 0);
  EXPECT_TRUE(joinMulticast(u, "239.255.0.1", "127.0.0.1"));
  EXPECT_FALSE(joinMulticast(u, "not-a-group", ""));
  closeSocket(u);
}

TEST(Socket, ErrorPredicatesTellTheCasesApart)
{
  // wouldBlock and interrupted must not both claim the same error, or a
  // retry loop turns into a spin.
#if defined(_WIN32)
  EXPECT_TRUE(wouldBlock(WSAEWOULDBLOCK));
  EXPECT_FALSE(interrupted(WSAEWOULDBLOCK));
#else
  EXPECT_TRUE(wouldBlock(EAGAIN));
  EXPECT_FALSE(interrupted(EAGAIN));
  EXPECT_TRUE(interrupted(EINTR));
  EXPECT_FALSE(wouldBlock(EINTR));
  EXPECT_FALSE(wouldBlock(ECONNRESET)) << "a broken socket is not an empty one";
#endif
}
