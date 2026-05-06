#include "flox-connectors/hyperliquid/hl_signer.h"

#include <cstring>
#include <optional>
#include <string>

#include <flox/log/log.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
#endif

namespace flox::hl
{

namespace
{

#ifdef _WIN32
struct WinsockInit
{
  WinsockInit()
  {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
  }
  ~WinsockInit() { WSACleanup(); }
};

static WinsockInit& ensureWinsock()
{
  static WinsockInit init;
  return init;
}

inline void close_socket(socket_t s) { ::closesocket(s); }
#else
inline void close_socket(socket_t s) { ::close(s); }
#endif

static bool send_all(socket_t fd, const void* p, size_t n)
{
  const char* c = static_cast<const char*>(p);
  while (n)
  {
#ifdef _WIN32
    int w = ::send(fd, c, static_cast<int>(n), 0);
#else
    ssize_t w = ::send(fd, c, n, MSG_NOSIGNAL);
#endif
    if (w <= 0)
    {
      return false;
    }
    c += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

static bool recv_all(socket_t fd, void* p, size_t n)
{
  char* c = static_cast<char*>(p);
  while (n)
  {
#ifdef _WIN32
    int r = ::recv(fd, c, static_cast<int>(n), 0);
#else
    ssize_t r = ::recv(fd, c, n, 0);
#endif
    if (r <= 0)
    {
      return false;
    }
    c += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

#ifndef _WIN32
static socket_t connect_unix(const char* path, int timeout_ms = 50)
{
  socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return INVALID_SOCK;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
  {
    close_socket(fd);
    return INVALID_SOCK;
  }
  return fd;
}
#endif

// Connect via TCP to localhost:port (cross-platform fallback)
static socket_t connect_tcp(uint16_t port, int timeout_ms = 50)
{
#ifdef _WIN32
  ensureWinsock();
#endif

  socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == INVALID_SOCK)
  {
    return INVALID_SOCK;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

#ifdef _WIN32
  DWORD tv = static_cast<DWORD>(timeout_ms);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    close_socket(fd);
    return INVALID_SOCK;
  }
  return fd;
}

}  // namespace

static std::string escape_json(const std::string& s)
{
  std::string o;
  o.reserve(s.size() + 16);
  for (char c : s)
  {
    switch (c)
    {
      case '\"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\b':
        o += "\\b";
        break;
      case '\f':
        o += "\\f";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        o += c;
        break;
    }
  }
  return o;
}

static std::string build_request_json(const HlSignParams& p)
{
  std::string j = "{";
  j += "\"action_json\":\"" + escape_json(p.actionJson) + "\"";
  j += ",\"nonce\":" + std::to_string(p.nonceMs);
  j += ",\"is_mainnet\":" + std::string(p.isMainnet ? "true" : "false");
  j += ",\"private_key\":\"" + escape_json(p.privateKeyHex) + "\"";
  j += ",\"active_pool\":";
  if (p.activePoolJson && !p.activePoolJson->empty())
  {
    j += *p.activePoolJson;
  }
  else
  {
    j += "null";
  }
  j += ",\"expires_after\":";
  if (p.expiresAfterMs)
  {
    j += std::to_string(*p.expiresAfterMs);
  }
  else
  {
    j += "null";
  }
  j += "}";
  return j;
}

// Default TCP port for hl_signerd (can be overridden via environment)
static constexpr uint16_t HL_SIGNER_DEFAULT_PORT = 19847;

std::optional<HlSig> hl_sign_with_sdk(const HlSignParams& p)
{
  const std::string req = build_request_json(p);

  socket_t fd = INVALID_SOCK;

#ifndef _WIN32
  // On POSIX, try Unix socket first (faster, default)
  fd = connect_unix("/dev/shm/hl_sign.sock", /*timeout_ms=*/50);
#endif

  // Fall back to TCP localhost (cross-platform)
  if (fd == INVALID_SOCK)
  {
    fd = connect_tcp(HL_SIGNER_DEFAULT_PORT, /*timeout_ms=*/50);
  }

  if (fd == INVALID_SOCK)
  {
    FLOX_LOG_ERROR("[HL] connect hl_signerd failed (tried Unix socket and TCP localhost:"
                   << HL_SIGNER_DEFAULT_PORT << ")");
    return std::nullopt;
  }

  uint32_t len = htonl(static_cast<uint32_t>(req.size()));
  if (!send_all(fd, &len, 4) || !send_all(fd, req.data(), req.size()))
  {
    close_socket(fd);
    FLOX_LOG_ERROR("[HL] send req failed");
    return std::nullopt;
  }

  uint32_t rlen_be = 0;
  if (!recv_all(fd, &rlen_be, 4))
  {
    close_socket(fd);
    return std::nullopt;
  }
  uint32_t rlen = ntohl(rlen_be);
  std::string out;
  out.resize(rlen);
  if (!recv_all(fd, out.data(), rlen))
  {
    close_socket(fd);
    return std::nullopt;
  }
  close_socket(fd);

  auto findv = [&](const char* key) -> std::string
  {
    std::string k = std::string("\"") + key + "\":";
    size_t pos = out.find(k);
    if (pos == std::string::npos)
    {
      return {};
    }
    pos += k.size();
    while (pos < out.size() && out[pos] == ' ')
    {
      ++pos;
    }
    if (pos < out.size() && out[pos] == '\"')
    {
      size_t e = out.find('\"', pos + 1);
      if (e == std::string::npos)
      {
        return {};
      }
      return out.substr(pos + 1, e - (pos + 1));
    }
    else
    {
      size_t e = out.find_first_of(",}\n\r ", pos);
      if (e == std::string::npos)
      {
        e = out.size();
      }
      return out.substr(pos, e - pos);
    }
  };

  std::string r = findv("r"), s = findv("s"), v = findv("v");
  if (r.empty() || s.empty() || v.empty())
  {
    FLOX_LOG_ERROR("[HL] signer bad json: " + out);
    return std::nullopt;
  }
  return HlSig{std::move(r), std::move(s), std::atoi(v.c_str())};
}

}  // namespace flox::hl
