/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/util/int/u256.h"

#include <cstdint>

namespace flox::whirlpool_tick
{

// Orca Whirlpool sqrt_price_from_tick_index, transcribed from the program: the
// Q64.64 sqrt price at a tick index, by bit-decomposition of |tick| against the
// program's constant table. Only the forward direction is needed (tick-group
// boundary prices for the adaptive fee); the current tick is read from the pool.

inline u256 q96() { return u256::fromDec("79228162514264337593543950336"); }  // 2^96
inline u256 q64() { return u256::fromDec("18446744073709551616"); }           // 2^64

inline u256 sqrtPriceFromTick(int32_t tick)
{
  // Positive path: ratio in Q96, mul_shift_96 per set bit, then >> 32 to Q64.64.
  static const char* posOdd = "79232123823359799118286999567";
  static const char* pos[18] = {
      "79236085330515764027303304731", "79244008939048815603706035061",
      "79259858533276714757314932305", "79291567232598584799939703904",
      "79355022692464371645785046466", "79482085999252804386437311141",
      "79736823300114093921829183326", "80248749790819932309965073892",
      "81282483887344747381513967011", "83390072131320151908154831281",
      "87770609709833776024991924138", "97234110755111693312479820773",
      "119332217159966728226237229890", "179736315981702064433883588727",
      "407748233172238350107850275304", "2098478828474011932436660412517",
      "55581415166113811149459800483533", "38992368544603139932233054999993551"};
  // Negative path: ratio in Q64.64, (ratio * c) >> 64 per set bit.
  static const char* negOdd = "18445821805675392311";
  static const char* neg[18] = {
      "18444899583751176498", "18443055278223354162", "18439367220385604838",
      "18431993317065449817", "18417254355718160513", "18387811781193591352",
      "18329067761203520168", "18212142134806087854", "17980523815641551639",
      "17526086738831147013", "16651378430235024244", "15030750278693429944",
      "12247334978882834399", "8131365268884726200", "3584323654723342297",
      "696457651847595233", "26294789957452057", "37481735321082"};

  if (tick >= 0)
  {
    u256 ratio = (tick & 1) ? u256::fromDec(posOdd) : q96();
    for (int k = 1; k < 19; ++k)
    {
      if (tick & (1 << k))
      {
        ratio = ratio * u256::fromDec(pos[k - 1]) / q96();
      }
    }
    return ratio / u256(4294967296ULL);  // >> 32
  }

  const uint32_t a = static_cast<uint32_t>(-static_cast<int64_t>(tick));
  u256 ratio = (a & 1u) ? u256::fromDec(negOdd) : q64();
  for (int k = 1; k < 19; ++k)
  {
    if (a & (1u << k))
    {
      ratio = ratio * u256::fromDec(neg[k - 1]) / q64();
    }
  }
  return ratio;
}

}  // namespace flox::whirlpool_tick
