/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Rendering a fixed-point raw for something outside the process: the control
 * plane's JSON replies and the Prometheus page.
 *
 * Both used to spell a money value with std::to_string(double), which is
 * specified as printf("%f") and therefore carries two properties neither
 * surface can afford. It renders six decimals, and every price, quantity and
 * rate in this venue is a raw at a 1e-8 scale, so a one-tick instrument
 * answered "tick":0.000000 and a 1e-7 funding rate published as 0.000000 --
 * the two numbers an operator reads the page for. And it follows the C
 * locale, so a process started with LC_NUMERIC set to a comma locale emitted
 * 0,00030000: invalid JSON for the operator's tooling, an unparseable sample
 * line for every scraper, from a setting that has nothing to do with the
 * venue.
 *
 * The rendering here never builds a float. It divides the raw by its scale
 * and writes the two halves as digits, so there is no precision to lose and
 * no locale to consult.
 */
#pragma once

#include <cstdint>
#include <string>

namespace flox::venue
{

// How many fractional digits a scale carries. Scales are powers of ten by
// contract (see scalesValid in ledger.h): 1e8 is eight digits, 1 is none.
inline constexpr int decimalsOfScale(int64_t scale) noexcept
{
  int digits = 0;
  for (int64_t s = scale; s > 1; s /= 10)
  {
    ++digits;
  }
  return digits;
}

// A raw at `scale`, with EVERY decimal that scale has -- 30'000 at 1e8 is
// "0.00030000", not "0.0003". A fixed width is what makes the two surfaces
// diffable and what makes the trailing digits an operator is checking visible
// at all; a reader that wants a number rather than a string parses it either
// way.
inline std::string fixedPointToStr(int64_t raw, int64_t scale)
{
  const bool negative = raw < 0;
  // Negate in unsigned so the most negative int64 has a magnitude at all.
  uint64_t mag = negative ? (~static_cast<uint64_t>(raw) + 1u) : static_cast<uint64_t>(raw);

  const uint64_t divisor = scale > 1 ? static_cast<uint64_t>(scale) : 1u;
  const int digits = decimalsOfScale(scale);

  // 20 digits of integral part, a sign, a dot, and up to 18 fractional ones.
  char buf[40];
  char* end = buf + sizeof buf;
  char* p = end;

  uint64_t frac = mag % divisor;
  uint64_t whole = mag / divisor;
  for (int i = 0; i < digits; ++i)
  {
    *--p = static_cast<char>('0' + static_cast<int>(frac % 10));
    frac /= 10;
  }
  if (digits > 0)
  {
    *--p = '.';
  }
  do
  {
    *--p = static_cast<char>('0' + static_cast<int>(whole % 10));
    whole /= 10;
  } while (whole > 0);
  if (negative)
  {
    *--p = '-';
  }
  return std::string(p, end);
}

}  // namespace flox::venue
