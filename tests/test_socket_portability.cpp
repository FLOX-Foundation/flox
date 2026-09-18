// The socket layer, exercised rather than declared. A portability header
// that nothing calls proves only that it compiles.

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#include <csignal>
#endif

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
  EXPECT_TRUE(bindTo(out, "127.0.0.1", 0));
  EXPECT_TRUE(listenOn(out, 4));
  return boundPort(out);
}

// accept, bounded. A listening socket becomes readable when a connection is
// waiting, which is the same wait every acceptor in the perimeter does. It
// also means a test whose connect quietly did not happen fails on the next
// line instead of parking in the kernel until someone kills it.
Handle acceptWithin(Handle srv, int ms, ::sockaddr_in* peer = nullptr)
{
  const PollResult r = pollRead(srv, ms);
  if (!r.readable)
  {
    return kInvalid;
  }
  return acceptOne(srv, peer);
}

Handle connectLoopback(uint16_t port)
{
  Handle c = openSocket(Kind::Tcp);
  EXPECT_TRUE(valid(c));
  EXPECT_TRUE(connectTo(c, "127.0.0.1", port));
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
  Handle accepted = acceptWithin(server, 2000);
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
  Handle accepted = acceptWithin(server, 2000);
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
  Handle accepted = acceptWithin(server, 2000);
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
  Handle accepted = acceptWithin(server, 2000);
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
  Handle accepted = acceptWithin(server, 2000);
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
  ASSERT_TRUE(bindTo(u, "", 0));
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

// bindTo asking for port 0 is the only way an ephemeral listener is built,
// and boundPort is the only way it learns which port it got. A layer that
// offered one without the other would be unusable for exactly that case.
TEST(Socket, BindToPortZeroGetsAFreePortAndBoundPortReportsIt)
{
  Handle s = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(s));
  ASSERT_TRUE(bindTo(s, "127.0.0.1", 0));
  const uint16_t p = boundPort(s);
  EXPECT_NE(p, 0) << "a bound socket has a port, whoever picked it";

  Handle other = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(other));
  EXPECT_FALSE(bindTo(other, "127.0.0.1", p)) << "the port is taken";
  closeSocket(other);
  closeSocket(s);
}

TEST(Socket, BindAndConnectRefuseAnAddressThatIsNotOne)
{
  Handle s = openSocket(Kind::Tcp);
  ASSERT_TRUE(valid(s));
  EXPECT_FALSE(bindTo(s, "not.an.address", 0));
  EXPECT_FALSE(connectTo(s, "300.1.2.3", 80));
  closeSocket(s);
}

TEST(Socket, AcceptGivesAConnectedHandleAndThePeerAddressWhenAsked)
{
  Handle srv = kInvalid;
  const uint16_t port = listenLoopback(srv);
  Handle cli = connectLoopback(port);

  ::sockaddr_in peer{};
  Handle acc = acceptWithin(srv, 2000, &peer);
  ASSERT_TRUE(valid(acc));
  EXPECT_EQ(peer.sin_family, AF_INET);
  EXPECT_NE(peer.sin_port, 0) << "the peer's ephemeral port, filled in by accept";

  const char msg[] = "through";
  ASSERT_GT(sendNoSignal(acc, msg, sizeof msg), 0);
  char buf[16] = {};
  EXPECT_EQ(receive(cli, buf, sizeof buf), static_cast<long>(sizeof msg));
  EXPECT_STREQ(buf, "through") << "the handle accept returned is the connected one";

  // The peer pointer is optional and omitting it must not change the handle.
  Handle cli2 = connectLoopback(port);
  Handle acc2 = acceptWithin(srv, 2000);
  EXPECT_TRUE(valid(acc2));

  closeSocket(acc2);
  closeSocket(cli2);
  closeSocket(acc);
  closeSocket(cli);
  closeSocket(srv);
}

TEST(Socket, DatagramsCarryTheirSenderBackToTheReceiver)
{
  Handle rx = openSocket(Kind::Udp);
  ASSERT_TRUE(valid(rx));
  ASSERT_TRUE(bindTo(rx, "127.0.0.1", 0));
  const uint16_t port = boundPort(rx);
  ASSERT_NE(port, 0);

  Handle tx = openSocket(Kind::Udp);
  ASSERT_TRUE(valid(tx));
  ASSERT_TRUE(bindTo(tx, "127.0.0.1", 0));
  const uint16_t txPort = boundPort(tx);

  ::sockaddr_in to{};
  ASSERT_TRUE(parseAddress("127.0.0.1", to, port));
  const char payload[] = "datagram";
  ASSERT_EQ(sendTo(tx, payload, sizeof payload, to), static_cast<long>(sizeof payload));

  char buf[32] = {};
  ::sockaddr_in from{};
  ASSERT_TRUE(setReceiveTimeout(rx, 1000));
  const long got = receiveFrom(rx, buf, sizeof buf, &from);
  EXPECT_EQ(got, static_cast<long>(sizeof payload));
  EXPECT_STREQ(buf, "datagram");
  EXPECT_EQ(ntohs(from.sin_port), txPort) << "the sender, reported back by the receive";

  closeSocket(tx);
  closeSocket(rx);
}

// Both of these take a differently-shaped argument on Windows than on POSIX,
// the same trap as the multicast TTL next to them.
TEST(Socket, MulticastInterfaceAcceptsASelectorAndRefusesAnAddressThatIsNotOne)
{
  Handle u = openSocket(Kind::Udp);
  ASSERT_TRUE(valid(u));
  EXPECT_TRUE(setMulticastInterface(u, "")) << "empty selector means let routing decide";
  EXPECT_TRUE(setMulticastInterface(u, "127.0.0.1"));
  EXPECT_FALSE(setMulticastInterface(u, "not.an.address"));
  closeSocket(u);
}

// This one is asserted by effect and not by return value, and the reason is
// worth writing down. Asking only "did setMulticastLoop return true" cannot
// tell a correct call from one that hands the kernel eight bytes where it
// wants one: this platform accepts both and reports success for both, so the
// mutation that swaps the argument's shape passed a green test. What the
// option is actually for is whether a sender hears its own multicast, so that
// is what is measured -- and a shape the platform does not accept fails here
// whatever setsockopt chose to report.
TEST(Socket, MulticastLoopDecidesWhetherASenderHearsItself)
{
  const std::string group = "239.255.77.19";
  const std::string iface = "127.0.0.1";

  Handle rx = openSocket(Kind::Udp);
  ASSERT_TRUE(valid(rx));
  ASSERT_TRUE(setReuseAddr(rx, true));
  ASSERT_TRUE(bindTo(rx, "", 0));
  const uint16_t port = boundPort(rx);
  ASSERT_NE(port, 0);
  ASSERT_TRUE(joinMulticast(rx, group, iface));
  ASSERT_TRUE(setReceiveTimeout(rx, 400));

  Handle tx = openSocket(Kind::Udp);
  ASSERT_TRUE(valid(tx));
  ASSERT_TRUE(setMulticastInterface(tx, iface));
  ASSERT_TRUE(setMulticastTtl(tx, 1));
  ::sockaddr_in to{};
  ASSERT_TRUE(parseAddress(group, to, port));

  const char msg[] = "loop";
  char buf[16] = {};

  ASSERT_TRUE(setMulticastLoop(tx, true));
  ASSERT_EQ(sendTo(tx, msg, sizeof msg, to), static_cast<long>(sizeof msg));
  EXPECT_EQ(receiveFrom(rx, buf, sizeof buf), static_cast<long>(sizeof msg))
      << "with loop on, the sending host receives its own datagram";

  ASSERT_TRUE(setMulticastLoop(tx, false));
  ASSERT_EQ(sendTo(tx, msg, sizeof msg, to), static_cast<long>(sizeof msg));
#if defined(__linux__)
  // Not asserted here, and the reason is a real difference rather than a
  // flake: the egress interface selected above is the loopback one, and on
  // Linux a datagram sent out of lo comes back because that is what lo does,
  // whatever the loop option says. The option governs the kernel's extra
  // copy, not delivery over an interface that is itself a loop. Selecting a
  // real NIC would make it observable, and CI runners have no such address
  // to count on. Drain whatever arrived so the socket is left clean.
  (void)receiveFrom(rx, buf, sizeof buf);
#else
  EXPECT_LT(receiveFrom(rx, buf, sizeof buf), 0)
      << "with loop off, it does not -- the receive times out instead";
#endif

  closeSocket(tx);
  closeSocket(rx);
}

// The poll result keeps "a signal arrived" apart from "the socket is broken"
// because the perimeter reacts to them in opposite ways: wait again, or drop
// the session. A quiet socket claims neither; a peer that left is reported as
// readable so the following receive returns zero and the session ends there,
// which is how every gateway here detects a disconnect.
TEST(Socket, PollTellsAQuietSocketFromADepartedPeer)
{
  Handle srv = kInvalid;
  const uint16_t port = listenLoopback(srv);
  Handle cli = connectLoopback(port);
  Handle acc = acceptWithin(srv, 2000);
  ASSERT_TRUE(valid(acc));

  const PollResult idle = pollRead(acc, 10);
  EXPECT_TRUE(idle.timedOut);
  EXPECT_FALSE(idle.error);
  EXPECT_FALSE(idle.interrupted);
  EXPECT_FALSE(idle.readable);

  closeSocket(cli);

  const PollResult gone = pollRead(acc, 1000);
  EXPECT_FALSE(gone.timedOut) << "a peer that left is an event, not silence";
  EXPECT_FALSE(gone.interrupted);
  EXPECT_TRUE(gone.readable || gone.hangup);

  char buf[8] = {};
  EXPECT_EQ(receive(acc, buf, sizeof buf), 0) << "and the read that follows says the peer is gone";

  closeSocket(acc);
  closeSocket(srv);
}

#if !defined(_WIN32)
// Windows has no interrupted call, so there is nothing to arrange there and
// the flag is always false -- see `interrupted` above. On POSIX the case is
// real and worth causing on purpose: a signal handler installed WITHOUT
// SA_RESTART makes the kernel return from poll early, and the layer must
// report that as "wait again" rather than as a broken socket. A perimeter
// that confused the two would drop live sessions whenever a signal arrived.
namespace
{
std::atomic<bool> g_signalled{false};
void onSignal(int) { g_signalled.store(true, std::memory_order_relaxed); }
}  // namespace

TEST(Socket, ASignalDuringAWaitIsReportedAsInterruptedNotAsAnError)
{
  struct sigaction sa
  {
  };
  sa.sa_handler = &onSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // deliberately not SA_RESTART: that is the whole point
  struct sigaction prev
  {
  };
  ASSERT_EQ(::sigaction(SIGUSR1, &sa, &prev), 0);

  Handle srv = kInvalid;
  const uint16_t port = listenLoopback(srv);
  Handle cli = connectLoopback(port);
  Handle acc = acceptWithin(srv, 2000);
  ASSERT_TRUE(valid(acc));

  const pthread_t self = ::pthread_self();
  std::thread waker(
      [self]
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::pthread_kill(self, SIGUSR1);
      });

  const auto started = std::chrono::steady_clock::now();
  const PollResult r = pollRead(acc, 5000);
  const auto waited = std::chrono::steady_clock::now() - started;
  waker.join();

  EXPECT_TRUE(g_signalled.load(std::memory_order_relaxed)) << "the signal really was delivered";
  EXPECT_TRUE(r.interrupted);
  EXPECT_FALSE(r.error) << "a signal is not a broken socket";
  EXPECT_FALSE(r.timedOut) << "and the wait did not run out";
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(), 4000)
      << "it returned early, which is what interrupted means";

  ::sigaction(SIGUSR1, &prev, nullptr);
  closeSocket(acc);
  closeSocket(cli);
  closeSocket(srv);
}
#endif
