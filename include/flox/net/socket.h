#pragma once

// The sockets a venue perimeter needs, in one place, so that the platforms'
// disagreements live here instead of at sixty call sites.
//
// This is not a portability layer for its own sake. Three of the
// differences below are silent: the code compiles on both sides and behaves
// differently, and each of them has its own named function here for exactly
// that reason.
//
//   Receive timeout. Both platforms spell the option SO_RCVTIMEO and take
//   different arguments for it -- a struct of seconds and microseconds on
//   POSIX, a count of milliseconds on Windows. Passing one where the other
//   is expected sets a timeout nobody intended, and nothing says so. The
//   perimeter leans on these timeouts for idle disconnects and for bounding
//   how long a lock is held, so getting it wrong does not fail, it drifts.
//
//   Suppressing SIGPIPE on a dead peer. macOS has a socket option, Linux has
//   a send flag, Windows has neither and needs nothing. Three answers to one
//   question, and the wrong one on macOS kills the process.
//
//   Non-blocking mode. POSIX reaches it through the file descriptor, Windows
//   through the socket handle, and the two APIs have nothing in common.
//
// What is NOT here: anything std::filesystem or the standard library already
// does portably, and any operation the perimeter does not use. A layer that
// wraps what nobody calls is a layer nobody maintains.

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace flox::net
{

#if defined(_WIN32)
using Handle = SOCKET;
inline constexpr Handle kInvalid = INVALID_SOCKET;
#else
using Handle = int;
inline constexpr Handle kInvalid = -1;
#endif

inline bool valid(Handle h) noexcept { return h != kInvalid; }

// Winsock must be started before any socket call and stopped after the last
// one. A static in a function gets both, once per process, whatever order
// translation units initialise in. On POSIX it costs one branch that the
// optimiser removes.
inline bool ensureStarted() noexcept
{
#if defined(_WIN32)
  struct Guard
  {
    bool ok{false};
    Guard()
    {
      WSADATA d;
      ok = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~Guard()
    {
      if (ok)
      {
        ::WSACleanup();
      }
    }
  };
  static Guard g;
  return g.ok;
#else
  return true;
#endif
}

// ---------------------------------------------------------------- errors

inline int lastError() noexcept
{
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

// The socket had nothing to give and is not broken. On POSIX this is two
// error numbers that may or may not be the same value; on Windows it is one
// with a different name.
inline bool wouldBlock(int e) noexcept
{
#if defined(_WIN32)
  return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT;
#else
  return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

// A signal arrived mid-call and the call should simply be retried. Windows
// has no such case, so this is always false there -- which is the honest
// answer, not an oversight.
inline bool interrupted(int e) noexcept
{
#if defined(_WIN32)
  (void)e;
  return false;
#else
  return e == EINTR;
#endif
}

// ---------------------------------------------------------------- lifetime

enum class Kind
{
  Tcp,
  Udp,
};

// Named openSocket/closeSocket rather than open/close: a caller that says
// `using namespace flox::net` would otherwise find these ambiguous with the
// POSIX functions of those names, and the error lands on them rather than
// here.
inline Handle openSocket(Kind k) noexcept
{
  if (!ensureStarted())
  {
    return kInvalid;
  }
  return ::socket(AF_INET, k == Kind::Tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
}

inline void closeSocket(Handle h) noexcept
{
  if (!valid(h))
  {
    return;
  }
#if defined(_WIN32)
  ::closesocket(h);
#else
  ::close(h);
#endif
}

// Both directions. The constant differs; the meaning does not.
inline void shutdownBoth(Handle h) noexcept
{
  if (!valid(h))
  {
    return;
  }
#if defined(_WIN32)
  ::shutdown(h, SD_BOTH);
#else
  ::shutdown(h, SHUT_RDWR);
#endif
}

// ---------------------------------------------------------------- options

namespace detail
{
#if defined(_WIN32)
using OptPtr = const char*;
using OptLen = int;
#else
using OptPtr = const void*;
using OptLen = ::socklen_t;
#endif

inline bool setOpt(Handle h, int level, int name, const void* v, size_t n) noexcept
{
  return ::setsockopt(h, level, name, static_cast<OptPtr>(v), static_cast<OptLen>(n)) == 0;
}
}  // namespace detail

inline bool setReuseAddr(Handle h, bool on) noexcept
{
  const int v = on ? 1 : 0;
  return detail::setOpt(h, SOL_SOCKET, SO_REUSEADDR, &v, sizeof v);
}

inline bool setNoDelay(Handle h, bool on) noexcept
{
  const int v = on ? 1 : 0;
  return detail::setOpt(h, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
}

inline bool setSendBufferBytes(Handle h, int bytes) noexcept
{
  return detail::setOpt(h, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
}

// The first of the three silent ones. Milliseconds in, whatever the platform
// wants out.
inline bool setReceiveTimeout(Handle h, int ms) noexcept
{
#if defined(_WIN32)
  const DWORD v = static_cast<DWORD>(ms);
  return detail::setOpt(h, SOL_SOCKET, SO_RCVTIMEO, &v, sizeof v);
#else
  ::timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return detail::setOpt(h, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

inline bool setSendTimeout(Handle h, int ms) noexcept
{
#if defined(_WIN32)
  const DWORD v = static_cast<DWORD>(ms);
  return detail::setOpt(h, SOL_SOCKET, SO_SNDTIMEO, &v, sizeof v);
#else
  ::timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return detail::setOpt(h, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

// The second. macOS answers with a socket option, Linux with a send flag
// (see sendNoSignal), Windows needs nothing at all.
inline bool suppressSigPipe(Handle h) noexcept
{
#if defined(SO_NOSIGPIPE)
  const int v = 1;
  return detail::setOpt(h, SOL_SOCKET, SO_NOSIGPIPE, &v, sizeof v);
#else
  (void)h;
  return true;
#endif
}

// The third. Nothing about these two APIs is alike.
inline bool setNonBlocking(Handle h, bool on) noexcept
{
#if defined(_WIN32)
  u_long v = on ? 1 : 0;
  return ::ioctlsocket(h, FIONBIO, &v) == 0;
#else
  const int fl = ::fcntl(h, F_GETFL, 0);
  if (fl < 0)
  {
    return false;
  }
  const int want = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
  return ::fcntl(h, F_SETFL, want) == 0;
#endif
}

inline bool setMulticastTtl(Handle h, int ttl) noexcept
{
#if defined(_WIN32)
  const DWORD v = static_cast<DWORD>(ttl);
  return detail::setOpt(h, IPPROTO_IP, IP_MULTICAST_TTL, &v, sizeof v);
#else
  const unsigned char v = static_cast<unsigned char>(ttl);
  return detail::setOpt(h, IPPROTO_IP, IP_MULTICAST_TTL, &v, sizeof v);
#endif
}

inline bool joinMulticast(Handle h, const std::string& group, const std::string& iface) noexcept
{
  ::ip_mreq r{};
  if (::inet_pton(AF_INET, group.c_str(), &r.imr_multiaddr) != 1)
  {
    return false;
  }
  if (iface.empty())
  {
    r.imr_interface.s_addr = htonl(INADDR_ANY);
  }
  else if (::inet_pton(AF_INET, iface.c_str(), &r.imr_interface) != 1)
  {
    return false;
  }
  return detail::setOpt(h, IPPROTO_IP, IP_ADD_MEMBERSHIP, &r, sizeof r);
}

// ---------------------------------------------------------------- transfer

#if defined(_WIN32)
using IoLen = int;
#else
using IoLen = size_t;
#endif

// Send that will not raise a signal on a peer that went away. On Linux the
// flag does it; on macOS suppressSigPipe did it at setup; on Windows there
// is nothing to suppress.
inline long sendNoSignal(Handle h, const void* p, size_t n) noexcept
{
#if defined(MSG_NOSIGNAL)
  return static_cast<long>(::send(h, static_cast<const char*>(p), static_cast<IoLen>(n), MSG_NOSIGNAL));
#else
  return static_cast<long>(::send(h, static_cast<const char*>(p), static_cast<IoLen>(n), 0));
#endif
}

inline long receive(Handle h, void* p, size_t n, bool peek = false) noexcept
{
  return static_cast<long>(::recv(h, static_cast<char*>(p), static_cast<IoLen>(n), peek ? MSG_PEEK : 0));
}

// ---------------------------------------------------------------- waiting

struct PollResult
{
  bool readable{false};
  bool hangup{false};
  bool error{false};
  bool timedOut{false};
};

// One socket, one wait. WSAPoll takes the same struct and the same flags as
// poll, so only the function's name differs -- which is why this is a thin
// wrapper and not a redesign.
inline PollResult pollRead(Handle h, int timeoutMs) noexcept
{
#if defined(_WIN32)
  ::WSAPOLLFD p{};
#else
  ::pollfd p{};
#endif
  p.fd = h;
  p.events = POLLIN;
#if defined(_WIN32)
  const int n = ::WSAPoll(&p, 1, timeoutMs);
#else
  const int n = ::poll(&p, 1, timeoutMs);
#endif
  PollResult r;
  if (n == 0)
  {
    r.timedOut = true;
    return r;
  }
  if (n < 0)
  {
    r.error = true;
    return r;
  }
  r.readable = (p.revents & POLLIN) != 0;
  r.hangup = (p.revents & POLLHUP) != 0;
  r.error = (p.revents & POLLERR) != 0;
  return r;
}

// ---------------------------------------------------------------- addresses

inline bool parseAddress(const std::string& ip, ::sockaddr_in& out, uint16_t port) noexcept
{
  out = ::sockaddr_in{};
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  if (ip.empty())
  {
    out.sin_addr.s_addr = htonl(INADDR_ANY);
    return true;
  }
  return ::inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
}

}  // namespace flox::net
