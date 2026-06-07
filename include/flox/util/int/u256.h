/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>

namespace flox
{

// Unsigned 256-bit integer for chain-faithful DEX math. EVM/Vyper contracts
// compute in uint256 with floor division; reproducing their output to the wei
// needs the same arithmetic, and C++ has no native 256-bit type. Little-endian
// 64-bit limbs (w[0] is least significant).
//
// Reads like math: the full operator set is here, so a contract formula
// transcribes directly. Two safety choices matter for fidelity:
//   - operator* is checked: it asserts in debug if the product exceeds 256 bits
//     (the contract would revert), so an overflow is a caught bug, not a silent
//     wraparound. A product that is meant to exceed 256 bits before a divide
//     goes through mulDiv / mulDivUp, which carry the full 512-bit intermediate.
//   - the wei<->human boundary is explicit: toDecimalString and pow10 take the
//     token decimals, so amounts of different decimals cannot be mixed silently.
struct u256
{
  std::array<uint64_t, 4> w{0, 0, 0, 0};

  constexpr u256() = default;
  constexpr u256(uint64_t v) : w{v, 0, 0, 0} {}

  constexpr bool isZero() const { return (w[0] | w[1] | w[2] | w[3]) == 0; }

  static u256 fromDec(const std::string& s)
  {
    u256 r;
    for (char c : s)
    {
      if (c == '_' || c == ' ')
      {
        continue;
      }
      if (c < '0' || c > '9')
      {
        throw std::invalid_argument("u256::fromDec: not a digit");
      }
      r = r * u256(10) + u256(static_cast<uint64_t>(c - '0'));
    }
    return r;
  }

  static u256 fromHex(const std::string& s)
  {
    u256 r;
    std::size_t i = (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
    for (; i < s.size(); ++i)
    {
      char c = s[i];
      if (c == '_')
      {
        continue;
      }
      int d;
      if (c >= '0' && c <= '9')
      {
        d = c - '0';
      }
      else if (c >= 'a' && c <= 'f')
      {
        d = c - 'a' + 10;
      }
      else if (c >= 'A' && c <= 'F')
      {
        d = c - 'A' + 10;
      }
      else
      {
        throw std::invalid_argument("u256::fromHex: not a hex digit");
      }
      r = r * u256(16) + u256(static_cast<uint64_t>(d));
    }
    return r;
  }

  std::string toDec() const
  {
    if (isZero())
    {
      return "0";
    }
    std::string out;
    u256 v = *this;
    const u256 ten(10);
    while (!v.isZero())
    {
      auto dm = divmod(v, ten);
      out.push_back(static_cast<char>('0' + dm.second.w[0]));
      v = dm.first;
    }
    return std::string(out.rbegin(), out.rend());
  }

  std::string toHex() const
  {
    if (isZero())
    {
      return "0x0";
    }
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (int limb = 3; limb >= 0; --limb)
    {
      for (int nib = 15; nib >= 0; --nib)
      {
        char c = digits[(w[limb] >> (nib * 4)) & 0xF];
        if (!out.empty() || c != '0')
        {
          out.push_back(c);
        }
      }
    }
    return "0x" + out;
  }

  // 10^n as a u256.
  static u256 pow10(unsigned n)
  {
    u256 r(1);
    const u256 ten(10);
    for (unsigned i = 0; i < n; ++i)
    {
      r = r * ten;
    }
    return r;
  }

  // Decimal string of a native amount given the token's decimals, e.g.
  // (1500000000000000000, 18) -> "1.5". The explicit decimals are the whole
  // point: a wei amount means nothing without them.
  std::string toDecimalString(unsigned decimals) const
  {
    std::string raw = toDec();
    if (decimals == 0)
    {
      return raw;
    }
    if (raw.size() <= decimals)
    {
      raw = std::string(decimals - raw.size() + 1, '0') + raw;
    }
    std::string whole = raw.substr(0, raw.size() - decimals);
    std::string frac = raw.substr(raw.size() - decimals);
    while (frac.size() > 1 && frac.back() == '0')
    {
      frac.pop_back();
    }
    if (frac == "0")
    {
      return whole;
    }
    return whole + "." + frac;
  }

  // --- comparisons ---
  friend bool operator==(const u256& a, const u256& b) { return a.w == b.w; }
  friend bool operator!=(const u256& a, const u256& b) { return !(a.w == b.w); }
  friend bool operator<(const u256& a, const u256& b)
  {
    for (int i = 3; i >= 0; --i)
    {
      if (a.w[i] != b.w[i])
      {
        return a.w[i] < b.w[i];
      }
    }
    return false;
  }
  friend bool operator>(const u256& a, const u256& b) { return b < a; }
  friend bool operator<=(const u256& a, const u256& b) { return !(b < a); }
  friend bool operator>=(const u256& a, const u256& b) { return !(a < b); }

  // --- add / sub (wrap mod 2^256, like the EVM) ---
  friend u256 operator+(const u256& a, const u256& b)
  {
    u256 r;
    unsigned __int128 carry = 0;
    for (int i = 0; i < 4; ++i)
    {
      unsigned __int128 s = static_cast<unsigned __int128>(a.w[i]) + b.w[i] + carry;
      r.w[i] = static_cast<uint64_t>(s);
      carry = s >> 64;
    }
    return r;
  }
  friend u256 operator-(const u256& a, const u256& b)
  {
    u256 r;
    __int128 borrow = 0;
    for (int i = 0; i < 4; ++i)
    {
      __int128 d = static_cast<__int128>(a.w[i]) - b.w[i] - borrow;
      if (d < 0)
      {
        d += (static_cast<__int128>(1) << 64);
        borrow = 1;
      }
      else
      {
        borrow = 0;
      }
      r.w[i] = static_cast<uint64_t>(d);
    }
    return r;
  }

  // 256x256 -> 512, the full product with no truncation.
  static std::array<uint64_t, 8> mulFull(const u256& a, const u256& b)
  {
    std::array<uint64_t, 8> r{};
    for (int i = 0; i < 4; ++i)
    {
      unsigned __int128 carry = 0;
      for (int j = 0; j < 4; ++j)
      {
        unsigned __int128 cur =
            static_cast<unsigned __int128>(a.w[i]) * b.w[j] + r[i + j] + carry;
        r[i + j] = static_cast<uint64_t>(cur);
        carry = cur >> 64;
      }
      r[i + 4] = static_cast<uint64_t>(r[i + 4] + carry);
    }
    return r;
  }

  // Checked multiply: the product must fit 256 bits (asserts in debug, the way
  // the contract would revert). Use mulDiv for a product meant to exceed 256.
  friend u256 operator*(const u256& a, const u256& b)
  {
    auto f = mulFull(a, b);
    assert((f[4] | f[5] | f[6] | f[7]) == 0 && "u256 multiply overflow");
    u256 r;
    r.w = {f[0], f[1], f[2], f[3]};
    return r;
  }

  static u256 shl1(u256 x)
  {
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i)
    {
      uint64_t nc = x.w[i] >> 63;
      x.w[i] = (x.w[i] << 1) | carry;
      carry = nc;
    }
    return x;
  }

  // Divide a 512-bit value by a u256, returning floor quotient and remainder by
  // restoring long division. The quotient is assumed to fit 256 bits (true for
  // u256/u256 and for mulDiv where the result fits). The intermediate remainder
  // can momentarily need a 257th bit, carried explicitly.
  static std::pair<u256, u256> divmod512(const std::array<uint64_t, 8>& num, const u256& den)
  {
    assert(!den.isZero() && "u256 division by zero");
    u256 q;
    u256 rem;
    for (int bit = 511; bit >= 0; --bit)
    {
      uint64_t carry = rem.w[3] >> 63;
      rem = shl1(rem);
      rem.w[0] |= (num[bit >> 6] >> (bit & 63)) & 1ull;
      if (carry != 0 || den <= rem)
      {
        rem = rem - den;
        if (bit < 256)
        {
          q.w[bit >> 6] |= (1ull << (bit & 63));
        }
      }
    }
    return {q, rem};
  }

  static std::pair<u256, u256> divmod(const u256& a, const u256& b)
  {
    std::array<uint64_t, 8> num{a.w[0], a.w[1], a.w[2], a.w[3], 0, 0, 0, 0};
    return divmod512(num, b);
  }

  friend u256 operator/(const u256& a, const u256& b) { return divmod(a, b).first; }
  friend u256 operator%(const u256& a, const u256& b) { return divmod(a, b).second; }

  friend std::ostream& operator<<(std::ostream& os, const u256& v) { return os << v.toDec(); }
};

// a * b / c with the full 512-bit intermediate, floored. The product may exceed
// 256 bits; only the result must fit. This is the primitive Uniswap and Curve
// math is written in terms of.
inline u256 mulDiv(const u256& a, const u256& b, const u256& c)
{
  return u256::divmod512(u256::mulFull(a, b), c).first;
}

// a * b / c rounded up, for the spots where a contract rounds against the user.
inline u256 mulDivUp(const u256& a, const u256& b, const u256& c)
{
  auto dm = u256::divmod512(u256::mulFull(a, b), c);
  return dm.second.isZero() ? dm.first : dm.first + u256(1);
}

inline u256 operator"" _u256(const char* s) { return u256::fromDec(s); }

}  // namespace flox
