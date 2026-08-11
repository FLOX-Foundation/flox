/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox/util/crypto.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace flox::ws
{

inline std::string acceptKey(const std::string& clientKey)
{
  static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const std::string s = clientKey + kGuid;
  const auto h = crypto::sha1(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  return crypto::base64(h.data(), h.size());
}

// Build the 101 Switching Protocols response from the client's request headers.
// Returns "" if no Sec-WebSocket-Key is present.
inline std::string handshakeResponse(const std::string& request)
{
  std::string lower(request.size(), '\0');
  for (size_t i = 0; i < request.size(); ++i)
  {
    lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(request[i])));
  }
  const std::string tag = "sec-websocket-key:";
  const auto pos = lower.find(tag);
  if (pos == std::string::npos)
  {
    return {};
  }
  size_t s = pos + tag.size();
  while (s < request.size() && request[s] == ' ')
  {
    ++s;
  }
  size_t e = request.find("\r\n", s);
  if (e == std::string::npos)
  {
    e = request.size();
  }
  std::string key = request.substr(s, e - s);
  while (!key.empty() && (key.back() == ' ' || key.back() == '\r'))
  {
    key.pop_back();
  }
  return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Accept: " +
         acceptKey(key) + "\r\n\r\n";
}

enum class Opcode : uint8_t
{
  Cont = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

// Build an unmasked server->client frame (FIN set).
inline std::vector<uint8_t> buildFrame(Opcode op, const uint8_t* payload, size_t n)
{
  std::vector<uint8_t> f;
  f.push_back(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(op)));
  if (n < 126)
  {
    f.push_back(static_cast<uint8_t>(n));
  }
  else if (n < 65536)
  {
    f.push_back(126);
    f.push_back(static_cast<uint8_t>(n >> 8));
    f.push_back(static_cast<uint8_t>(n));
  }
  else
  {
    f.push_back(127);
    for (int i = 7; i >= 0; --i)
    {
      f.push_back(static_cast<uint8_t>(n >> (i * 8)));
    }
  }
  f.insert(f.end(), payload, payload + n);
  return f;
}

// Largest payload we will buffer/allocate for one frame. Venue messages are
// tiny; this only has to be generous enough for a batched snapshot. It caps the
// per-connection buffer and, crucially, bounds the resize() below.
inline constexpr uint64_t kMaxFramePayload = 16u << 20;  // 16 MiB
// Sentinel return from parseFrame meaning "protocol violation -- close the
// connection" (distinct from 0 = "need more bytes"). Without it, an oversized or
// overflowing length would either crash on resize or spin reading forever.
inline constexpr size_t kParseError = static_cast<size_t>(-1);

// Parse one frame; returns bytes consumed (0 if incomplete, kParseError on a
// protocol violation). Unmasks client payloads. When `finOut` is non-null it
// receives the FIN bit, so the caller can reassemble fragmented messages
// (RFC 6455 5.4); a caller that ignores it must treat every frame as final.
// RSV1-3 must be zero (no extension is negotiated) -- a set RSV bit is a
// protocol violation, not silently ignored.
inline size_t parseFrame(const uint8_t* p, size_t n, Opcode& op, std::vector<uint8_t>& payload,
                         bool* finOut = nullptr)
{
  if (n < 2)
  {
    return 0;
  }
  if ((p[0] & 0x70) != 0)
  {
    return kParseError;  // RSV1/2/3 set without a negotiated extension
  }
  const bool fin = (p[0] & 0x80) != 0;
  op = static_cast<Opcode>(p[0] & 0x0F);
  // Control frames (Close/Ping/Pong) must not be fragmented.
  if ((static_cast<uint8_t>(op) & 0x08) != 0 && !fin)
  {
    return kParseError;
  }
  const bool masked = (p[1] & 0x80) != 0;
  uint64_t len = p[1] & 0x7F;
  size_t off = 2;
  if (len == 126)
  {
    if (n < 4)
    {
      return 0;
    }
    len = (uint64_t(p[2]) << 8) | p[3];
    off = 4;
  }
  else if (len == 127)
  {
    if (n < 10)
    {
      return 0;
    }
    len = 0;
    for (int i = 0; i < 8; ++i)
    {
      len = (len << 8) | p[2 + i];
    }
    off = 10;
  }
  // Reject an absurd/hostile length BEFORE any allocation. Also makes the
  // completeness check below safe: len is bounded, so off + len cannot wrap.
  if (len > kMaxFramePayload)
  {
    return kParseError;
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked)
  {
    if (n < off + 4)
    {
      return 0;
    }
    for (int i = 0; i < 4; ++i)
    {
      mask[i] = p[off + i];
    }
    off += 4;
  }
  // off <= n holds here (each length/mask branch validated it), so n - off does
  // not underflow; comparing len against it avoids the off + len overflow that
  // a 64-bit extended length (code 127) could otherwise use to defeat the guard.
  if (len > static_cast<uint64_t>(n - off))
  {
    return 0;
  }
  payload.resize(len);
  for (uint64_t i = 0; i < len; ++i)
  {
    payload[i] = p[off + i] ^ (masked ? mask[i & 3] : 0);
  }
  if (finOut != nullptr)
  {
    *finOut = fin;
  }
  return off + len;
}

}  // namespace flox::ws
