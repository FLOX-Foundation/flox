/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace flox::crypto
{

// ---- SHA-1 ----
inline std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len)
{
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  auto rol = [](uint32_t x, int c)
  { return (x << c) | (x >> (32 - c)); };

  std::vector<uint8_t> msg(data, data + len);
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56)
  {
    msg.push_back(0);
  }
  for (int i = 7; i >= 0; --i)
  {
    msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));
  }

  for (size_t off = 0; off < msg.size(); off += 64)
  {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
    {
      w[i] = (uint32_t(msg[off + i * 4]) << 24) | (uint32_t(msg[off + i * 4 + 1]) << 16) |
             (uint32_t(msg[off + i * 4 + 2]) << 8) | uint32_t(msg[off + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i)
    {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i)
    {
      uint32_t f, k;
      if (i < 20)
      {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      }
      else if (i < 40)
      {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      }
      else if (i < 60)
      {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      }
      else
      {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  std::array<uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
    }
  }
  return out;
}

// ---- SHA-256 ----
inline std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len)
{
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto ror = [](uint32_t x, int c)
  { return (x >> c) | (x << (32 - c)); };

  std::vector<uint8_t> msg(data, data + len);
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56)
  {
    msg.push_back(0);
  }
  for (int i = 7; i >= 0; --i)
  {
    msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));
  }

  for (size_t off = 0; off < msg.size(); off += 64)
  {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
      w[i] = (uint32_t(msg[off + i * 4]) << 24) | (uint32_t(msg[off + i * 4 + 1]) << 16) |
             (uint32_t(msg[off + i * 4 + 2]) << 8) | uint32_t(msg[off + i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
      const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i)
    {
      const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  std::array<uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
    }
  }
  return out;
}

// ---- HMAC-SHA256 ----
inline std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg,
                                          size_t msgLen)
{
  uint8_t k[64] = {0};
  if (keyLen > 64)
  {
    const auto kh = sha256(key, keyLen);
    std::memcpy(k, kh.data(), 32);
  }
  else
  {
    std::memcpy(k, key, keyLen);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i)
  {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  std::vector<uint8_t> inner(ipad, ipad + 64);
  inner.insert(inner.end(), msg, msg + msgLen);
  const auto ih = sha256(inner.data(), inner.size());
  std::vector<uint8_t> outer(opad, opad + 64);
  outer.insert(outer.end(), ih.begin(), ih.end());
  return sha256(outer.data(), outer.size());
}

// ---- base64 ----
inline std::string base64(const uint8_t* data, size_t len)
{
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 3 <= len; i += 3)
  {
    const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += tbl[n & 63];
  }
  if (len - i == 1)
  {
    const uint32_t n = uint32_t(data[i]) << 16;
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += "==";
  }
  else if (len - i == 2)
  {
    const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += "=";
  }
  return out;
}

inline std::string toHex(const uint8_t* data, size_t len)
{
  static const char* h = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i)
  {
    out += h[data[i] >> 4];
    out += h[data[i] & 15];
  }
  return out;
}

}  // namespace flox::crypto
