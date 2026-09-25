/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Security test for the Hyperliquid signing client. hl_sign_with_sdk puts the
 * raw private key into its request body, so the transport it picks decides who
 * can read that key:
 *
 *   - the shipped daemon (connectors/utils/hl_signerd.py) listens on a 0600
 *     Unix socket only, and never on TCP;
 *   - the client nevertheless falls back to TCP 127.0.0.1:19847, where any
 *     local process -- any user on the box -- can bind first and harvest the
 *     key, with no authentication of either end;
 *   - the reply length is attacker-controlled and goes straight into
 *     std::string::resize, so whoever holds that socket also picks the size of
 *     an allocation in the trading process.
 *
 * No network beyond loopback and a Unix socket the test creates itself.
 */

#include "flox-connectors/hyperliquid/hl_signer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace flox::hl;

namespace
{

// The port the client hardcodes as its cross-platform fallback.
constexpr uint16_t kSignerTcpPort = 19847;

// Marker that must never leave the process over an unauthenticated transport.
constexpr const char* kSecretKey =
    "0xdeadbeefcafebabe00112233445566778899aabbccddeeff0011223344556677";

#ifndef _WIN32

// A length-prefixed request/response server. Accepts one connection, records
// the request body, then writes the canned reply header (and body, if any).
class FramedServer
{
 public:
  // Reply header written back to the client. bodyIsTruncated lets a test
  // declare a length far larger than what it actually sends.
  struct Reply
  {
    uint32_t declaredLen{0};
    std::string body;
  };

  FramedServer() = default;

  ~FramedServer() { stop(); }

  bool listenTcp(uint16_t port)
  {
    _fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_fd < 0)
    {
      return false;
    }
    int one = 1;
    ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(_fd, 8) != 0)
    {
      ::close(_fd);
      _fd = -1;
      return false;
    }
    return true;
  }

  bool listenUnix(const std::string& path, mode_t mode)
  {
    ::unlink(path.c_str());
    _fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (_fd < 0)
    {
      return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
      ::close(_fd);
      _fd = -1;
      return false;
    }
    if (::chmod(path.c_str(), mode) != 0 || ::listen(_fd, 8) != 0)
    {
      ::close(_fd);
      _fd = -1;
      return false;
    }
    _unixPath = path;
    return true;
  }

  void serveOnce(Reply reply)
  {
    _thread = std::thread(
        [this, reply]()
        {
          pollfd p{_fd, POLLIN, 0};
          if (::poll(&p, 1, 3000) <= 0)
          {
            return;
          }
          int c = ::accept(_fd, nullptr, nullptr);
          if (c < 0)
          {
            return;
          }
          _accepted.store(true);

          uint32_t lenBe = 0;
          if (readExactly(c, &lenBe, 4))
          {
            const uint32_t len = ntohl(lenBe);
            if (len > 0 && len < (1u << 20))
            {
              std::string body(len, '\0');
              if (readExactly(c, body.data(), len))
              {
                _request = std::move(body);
              }
            }
          }

          const uint32_t declared = htonl(reply.declaredLen);
          (void)::send(c, &declared, 4, 0);
          if (!reply.body.empty())
          {
            (void)::send(c, reply.body.data(), reply.body.size(), 0);
          }
          // Hold the connection open briefly so the client reads the header
          // before the peer disappears.
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          ::close(c);
        });
  }

  void stop()
  {
    if (_thread.joinable())
    {
      _thread.join();
    }
    if (_fd >= 0)
    {
      ::close(_fd);
      _fd = -1;
    }
    if (!_unixPath.empty())
    {
      ::unlink(_unixPath.c_str());
      _unixPath.clear();
    }
  }

  bool accepted() const { return _accepted.load(); }
  const std::string& request() const { return _request; }

 private:
  static bool readExactly(int fd, void* dst, size_t n)
  {
    char* c = static_cast<char*>(dst);
    while (n)
    {
      const ssize_t r = ::recv(fd, c, n, 0);
      if (r <= 0)
      {
        return false;
      }
      c += r;
      n -= static_cast<size_t>(r);
    }
    return true;
  }

  int _fd{-1};
  std::string _unixPath;
  std::thread _thread;
  std::atomic<bool> _accepted{false};
  std::string _request;
};

HlSignParams params()
{
  HlSignParams p;
  p.actionJson = R"({"type":"order"})";
  p.nonceMs = 1700000000000LL;
  p.privateKeyHex = kSecretKey;
  p.isMainnet = true;
  return p;
}

std::string tempSocketPath(const char* name)
{
  auto dir = std::filesystem::temp_directory_path() / "flox_hl_signer_test";
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

// Peak resident set of this process, in bytes.
size_t peakRssBytes()
{
  rusage ru{};
  ::getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
  return static_cast<size_t>(ru.ru_maxrss);
#else
  return static_cast<size_t>(ru.ru_maxrss) * 1024u;
#endif
}

std::string validSignatureReply() { return R"({"r":"0x11","s":"0x22","v":27})"; }

#endif  // !_WIN32

}  // namespace

#ifndef _WIN32

// The finding: with no Unix socket present the client falls back to loopback
// TCP and writes the raw private key to whoever answers. A local attacker only
// has to bind 19847 first. The signer must refuse that transport -- the key
// never goes onto an unauthenticated socket.
TEST(HlSignerTransport, NeverSendsThePrivateKeyOverLoopbackTcp)
{
  FramedServer tcp;
  ASSERT_TRUE(tcp.listenTcp(kSignerTcpPort))
      << "could not bind 127.0.0.1:" << kSignerTcpPort << " for the test";
  tcp.serveOnce({0, {}});

  const auto sig = hl_sign_with_sdk(params());

  tcp.stop();

  EXPECT_EQ(tcp.request().find(kSecretKey), std::string::npos)
      << "the private key was written to an unauthenticated loopback TCP socket";
  EXPECT_FALSE(tcp.accepted())
      << "the signer must not speak to a TCP endpoint it cannot authenticate";
  EXPECT_FALSE(sig.has_value()) << "no trusted signer was reachable, so signing must fail loudly";
}

// The signer must honour a configured Unix socket and must only trust one that
// is private to this user. The daemon creates it 0600; anything wider means
// another local user can either read the key or impersonate the signer.
//
// needs: a way to point the signer at a socket path, e.g. the environment
// variable FLOX_HL_SIGNER_SOCKET read by hl_sign_with_sdk (today the path is
// hardcoded to /dev/shm/hl_sign.sock, which does not even exist on macOS, so
// every signature on that platform takes the TCP path above).
TEST(HlSignerTransport, OnlyTrustsAPrivateUnixSocket)
{
  const std::string path = tempSocketPath("sign.sock");
  ::setenv("FLOX_HL_SIGNER_SOCKET", path.c_str(), 1);

  {
    FramedServer priv;
    ASSERT_TRUE(priv.listenUnix(path, 0600));
    const std::string body = validSignatureReply();
    priv.serveOnce({static_cast<uint32_t>(body.size()), body});

    const auto sig = hl_sign_with_sdk(params());
    priv.stop();

    ASSERT_TRUE(sig.has_value()) << "a 0600 Unix socket is the trusted transport and must be used";
    EXPECT_EQ(sig->r, "0x11");
    EXPECT_EQ(sig->s, "0x22");
    EXPECT_EQ(sig->v, 27);
    EXPECT_NE(priv.request().find(kSecretKey), std::string::npos);
  }

  {
    FramedServer wide;
    ASSERT_TRUE(wide.listenUnix(path, 0666));
    const std::string body = validSignatureReply();
    wide.serveOnce({static_cast<uint32_t>(body.size()), body});

    const auto sig = hl_sign_with_sdk(params());
    wide.stop();

    EXPECT_FALSE(sig.has_value()) << "a world-accessible signer socket must not be trusted";
    EXPECT_EQ(wide.request().find(kSecretKey), std::string::npos)
        << "the private key was written to a socket any local user can open";
  }

  ::unsetenv("FLOX_HL_SIGNER_SOCKET");
}

// A symlink is not a socket, however private the thing it points at is: the
// path is checked with lstat so a link this user owns, pointing at a socket
// someone else controls, is refused rather than followed. The link here points
// at a socket the test owns, which is exactly the case a check that follows
// symlinks accepts.
TEST(HlSignerTransport, RefusesASymlinkStandingInForTheSignerSocket)
{
  const std::string real = tempSocketPath("sign_real.sock");
  const std::string link = tempSocketPath("sign_link.sock");
  ::unlink(link.c_str());

  FramedServer priv;
  ASSERT_TRUE(priv.listenUnix(real, 0600));
  ASSERT_EQ(::symlink(real.c_str(), link.c_str()), 0) << "could not create the test symlink";

  ::setenv("FLOX_HL_SIGNER_SOCKET", link.c_str(), 1);
  const std::string body = validSignatureReply();
  priv.serveOnce({static_cast<uint32_t>(body.size()), body});

  const auto sig = hl_sign_with_sdk(params());

  priv.stop();
  ::unlink(link.c_str());
  ::unsetenv("FLOX_HL_SIGNER_SOCKET");

  EXPECT_FALSE(sig.has_value()) << "a symlinked signer path must not be trusted";
  EXPECT_FALSE(priv.accepted()) << "the signer connected through a path it never checked";
  EXPECT_EQ(priv.request().find(kSecretKey), std::string::npos)
      << "the private key was written through a symlink whose target was never verified";
}

// Group access is access. A 0660 socket hands the key to every member of its
// group, which on a shared host is not the same set of people as "this user":
// the daemon creates it 0600 and anything wider is refused.
TEST(HlSignerTransport, RefusesAGroupAccessibleSocket)
{
  const std::string path = tempSocketPath("sign_group.sock");
  ::setenv("FLOX_HL_SIGNER_SOCKET", path.c_str(), 1);

  FramedServer group;
  ASSERT_TRUE(group.listenUnix(path, 0660));
  const std::string body = validSignatureReply();
  group.serveOnce({static_cast<uint32_t>(body.size()), body});

  const auto sig = hl_sign_with_sdk(params());

  group.stop();
  ::unsetenv("FLOX_HL_SIGNER_SOCKET");

  EXPECT_FALSE(sig.has_value()) << "a group-accessible signer socket must not be trusted";
  EXPECT_EQ(group.request().find(kSecretKey), std::string::npos)
      << "the private key was written to a socket every member of its group can open";
}

// Every bit outside the owner's is a way in, read included: a socket another
// user can only read still lets that user see the signing traffic, and the
// key is in it. The mode here sets no group bits at all, so nothing but the
// other-read bit can refuse it -- 0664 would be refused by its group bits
// alone and would not say whether other-read is checked.
TEST(HlSignerTransport, RefusesAnOtherReadableSocket)
{
  const std::string path = tempSocketPath("sign_other_read.sock");
  ::setenv("FLOX_HL_SIGNER_SOCKET", path.c_str(), 1);

  FramedServer readable;
  ASSERT_TRUE(readable.listenUnix(path, 0604));
  const std::string body = validSignatureReply();
  readable.serveOnce({static_cast<uint32_t>(body.size()), body});

  const auto sig = hl_sign_with_sdk(params());

  readable.stop();
  ::unsetenv("FLOX_HL_SIGNER_SOCKET");

  EXPECT_FALSE(sig.has_value()) << "an other-readable signer socket must not be trusted";
  EXPECT_EQ(readable.request().find(kSecretKey), std::string::npos)
      << "the private key was written to a socket another user can read";
}

// The finding: the reply's 4-byte length header goes straight into
// std::string::resize with no ceiling, so the peer picks the allocation size.
// A signature response is a few hundred bytes; anything far above that is a
// protocol violation and must be refused before a single byte is reserved.
//
// needs: a documented ceiling on the response length (a few KiB is generous
// for {"r","s","v"}) and a refusal above it.
TEST(HlSignerTransport, RefusesAResponseLengthAboveTheBound)
{
  const std::string path = tempSocketPath("sign_big.sock");
  ::setenv("FLOX_HL_SIGNER_SOCKET", path.c_str(), 1);

  FramedServer unixSrv;
  ASSERT_TRUE(unixSrv.listenUnix(path, 0600));
  // Declare half a gigabyte and send no body at all.
  constexpr uint32_t kDeclared = 512u * 1024u * 1024u;
  unixSrv.serveOnce({kDeclared, {}});

  // The TCP fallback is live too, so today's client reaches the same oversized
  // header by whichever route it picks; the bound must hold on both.
  FramedServer tcpSrv;
  const bool tcpUp = tcpSrv.listenTcp(kSignerTcpPort);
  if (tcpUp)
  {
    tcpSrv.serveOnce({kDeclared, {}});
  }

  const size_t rssBefore = peakRssBytes();
  const auto sig = hl_sign_with_sdk(params());
  const size_t rssAfter = peakRssBytes();

  unixSrv.stop();
  if (tcpUp)
  {
    tcpSrv.stop();
  }
  ::unsetenv("FLOX_HL_SIGNER_SOCKET");

  EXPECT_FALSE(sig.has_value());

  const size_t grew = rssAfter > rssBefore ? rssAfter - rssBefore : 0;
  EXPECT_LT(grew, 64u * 1024u * 1024u) << "the signer allocated " << (grew / (1024 * 1024))
                                       << " MiB for a response length the peer made up";
}

// Control (green today): with nothing listening anywhere the call fails and
// returns no signature rather than hanging or inventing one.
TEST(HlSignerTransport, NoSignerReachableFailsCleanly)
{
  const auto sig = hl_sign_with_sdk(params());
  EXPECT_FALSE(sig.has_value());
}

#endif  // !_WIN32
