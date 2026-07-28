/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Perpetual funding RATE computation (live side). Complements
 * backtest/funding_schedule.h, which REPLAYS a recorded rate tape: this
 * computes the rate from live mark/index. The funding
 * rate is the premium index (mark vs index) plus a clamped interest component,
 * capped; at each funding interval longs pay shorts (or vice-versa) in
 * proportion to position notional. Zero-sum across the book.
 *
 * Two paths: rate(mark,index) is a single snapshot; the interval TWAP path
 * (sample() across the interval, settle on intervalRate()) is manipulation-
 * resistant -- a momentary dislocation cannot swing the payment. Pair sample()
 * with the mark/index feed (index_feed.h).
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace flox::venue
{

struct FundingParams
{
  double interestRate{0.0001};  // baseline interest component per interval (1bp)
  double clampBand{0.0005};     // clamp on (interest - premium) (5bp)
  double cap{0.0075};           // max |funding rate| per interval (0.75%)
};

struct PerpPosition
{
  uint64_t account{};
  double signedQty{};  // + long, - short
};

struct FundingCharge
{
  uint64_t account{};
  double amount{};  // signed: negative = account pays, positive = account receives
};

class FundingCalculator
{
 public:
  explicit FundingCalculator(FundingParams p = {}) : p_(p) {}

  // funding = premium + clamp(interest - premium, ±band), capped at ±cap.
  double rate(double mark, double index) const
  {
    if (index <= 0.0)
    {
      return 0.0;
    }
    return rateFromPremium(premiumOf(mark, index));
  }

  // --- interval TWAP path (manipulation-resistant) ---
  // Real venues do not funding off a single snapshot: a momentary dislocation
  // would swing the payment. Sample the premium repeatedly across the interval,
  // then settle on the average. Feed each sample from the mark/index feed.
  void sample(double mark, double index)
  {
    if (index <= 0.0)
    {
      return;
    }
    premiumSum_ += premiumOf(mark, index);
    ++samples_;
  }

  // Rate from the time-averaged premium accumulated via sample(). Falls back to
  // the interest rate when no samples were taken.
  double intervalRate() const
  {
    if (samples_ == 0)
    {
      return rateFromPremium(0.0);
    }
    return rateFromPremium(premiumSum_ / static_cast<double>(samples_));
  }

  void resetSamples()
  {
    premiumSum_ = 0.0;
    samples_ = 0;
  }

  std::size_t sampleCount() const noexcept { return samples_; }

  // Settle funding at `mark`. Longs pay when the rate is positive; the total
  // across a balanced book nets to zero.
  std::vector<FundingCharge> settle(const std::vector<PerpPosition>& pos, double mark,
                                    double index) const
  {
    const double r = rate(mark, index);
    std::vector<FundingCharge> out;
    out.reserve(pos.size());
    for (const auto& p : pos)
    {
      out.push_back(FundingCharge{p.account, -p.signedQty * mark * r});
    }
    return out;
  }

  FundingParams params() const noexcept { return p_; }

 private:
  static double premiumOf(double mark, double index)
  {
    return index <= 0.0 ? 0.0 : (mark - index) / index;
  }

  double rateFromPremium(double premium) const
  {
    double c = p_.interestRate - premium;
    c = std::max(-p_.clampBand, std::min(p_.clampBand, c));
    const double f = premium + c;
    return std::max(-p_.cap, std::min(p_.cap, f));
  }

  FundingParams p_;
  double premiumSum_{0.0};
  std::size_t samples_{0};
};

}  // namespace flox::venue
