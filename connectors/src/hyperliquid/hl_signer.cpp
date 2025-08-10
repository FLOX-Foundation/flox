#include "flox-connectors/hyperliquid/hl_signer.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <optional>
#include <string>

#include <flox/log/log.h>

namespace flox::hl
{

static bool send_all(int fd, const void* p, size_t n)
{
  const char* c = static_cast<const char*>(p);
  while (n)
  {
    ssize_t w = ::send(fd, c, n, MSG_NOSIGNAL);
    if (w <= 0)
    {
      return false;
    }
    c += w;
    n -= size_t(w);
  }
  return true;
}

static bool recv_all(int fd, void* p, size_t n)
{
  char* c = static_cast<char*>(p);
  while (n)
  {
    ssize_t r = ::recv(fd, c, n, 0);
    if (r <= 0)
    {
      return false;
    }
    c += r;
    n -= size_t(r);
  }
  return true;
}

static int connect_unix(const char* path, int timeout_ms = 50)
{
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
  {
    ::close(fd);
    return -1;
  }
  return fd;
}

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

std::optional<HlSig> hl_sign_with_sdk(const HlSignParams& p)
{
  const std::string req = build_request_json(p);

  int fd = connect_unix("/dev/shm/hl_sign.sock", /*timeout_ms=*/50);
  if (fd < 0)
  {
    FLOX_LOG_ERROR("[HL] connect hl_signerd failed");
    return std::nullopt;
  }

  uint32_t len = htonl(static_cast<uint32_t>(req.size()));
  if (!send_all(fd, &len, 4) || !send_all(fd, req.data(), req.size()))
  {
    ::close(fd);
    FLOX_LOG_ERROR("[HL] send req failed");
    return std::nullopt;
  }

  uint32_t rlen_be = 0;
  if (!recv_all(fd, &rlen_be, 4))
  {
    ::close(fd);
    return std::nullopt;
  }
  uint32_t rlen = ntohl(rlen_be);
  std::string out;
  out.resize(rlen);
  if (!recv_all(fd, out.data(), rlen))
  {
    ::close(fd);
    return std::nullopt;
  }
  ::close(fd);

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
