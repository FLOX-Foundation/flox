/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Quote flow to develop a client against: a ladder and a stepping mid.
 *
 * Both are driven by a seed and nothing else -- no clock, no global state, no
 * thread. The same seed produces the same commands in the same order, which
 * is what makes a failure in a client reproducible instead of a story about
 * what the market was doing at the time. That property is the whole point of
 * generating flow rather than replaying a capture.
 *
 * They emit InboundCommands; where those go -- straight into an engine, or
 * over a gateway -- is the caller's business.
 */
#pragma once

#include "flox-venue/messages.h"

#include <cstdint>
#include <vector>

namespace flox::venue::script
{

// A deterministic generator, written out rather than taken from <random>:
// libstdc++ and libc++ disagree on what a distribution does with the same
// engine, so a seed would stop meaning the same thing across platforms --
// and "reproducible by seed" is the only thing this file promises.
class Rng
{
 public:
  explicit Rng(uint64_t seed) : s_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  uint64_t next()
  {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return s_;
  }

  // [0, n)
  uint64_t below(uint64_t n) { return n == 0 ? 0 : next() % n; }

  // true with probability num/den
  bool chance(uint64_t num, uint64_t den) { return below(den) < num; }

 private:
  uint64_t s_;
};

// N levels a side around a mid, re-quoted every round. Sizes wander; the mid
// steps a tick now and then. The ids are stable per level, so a level is
// replaced rather than accumulated -- which is what a real maker does and
// what keeps the book from growing without bound.
class LadderQuoter
{
 public:
  struct Config
  {
    SymbolId symbol{1};
    uint64_t account{1};
    int levels{5};
    int64_t midRaw{100'00000000};  // 100.0 at 1e8
    int64_t tickRaw{1'000000};     // 0.01
    int64_t sizeRaw{100'000'000};  // 1.0
    // How often the mid steps, as num/den. 1/5 means one round in five.
    uint64_t stepNum{1};
    uint64_t stepDen{5};
    bool lastLook{false};
    OrderId firstId{1'000'000};
  };

  LadderQuoter(Config cfg, uint64_t seed) : cfg_(cfg), rng_(seed), midRaw_(cfg.midRaw) {}

  // One round of quotes. Appends rather than returning, so a caller batching
  // several instruments pays one allocation.
  //
  // Each level goes out as a Quote, not a NewOrder: Quote is the primitive
  // built for replacing a two-sided quote by id, and it is what a maker
  // actually uses. The first version of this emitted NewOrder with stable ids
  // and looked right -- every round after the first came back
  // DuplicateOrderId, so nothing was quoted at all and a throughput benchmark
  // was measuring the rejection path. The test that was supposed to catch it
  // checked the ids this class produced rather than what the venue did with
  // them.
  void next(std::vector<InboundCommand>& out)
  {
    if (rng_.chance(cfg_.stepNum, cfg_.stepDen))
    {
      midRaw_ += rng_.chance(1, 2) ? cfg_.tickRaw : -cfg_.tickRaw;
    }
    for (int i = 0; i < cfg_.levels; ++i)
    {
      const int64_t off = cfg_.tickRaw * static_cast<int64_t>(i + 1);
      // A size that wanders within a factor of two, so a client sees changes
      // rather than a constant.
      const int64_t size = cfg_.sizeRaw + static_cast<int64_t>(
                                              rng_.below(static_cast<uint64_t>(cfg_.sizeRaw)));
      Quote q;
      q.bidId = cfg_.firstId + static_cast<OrderId>(2 * i);
      q.askId = cfg_.firstId + static_cast<OrderId>(2 * i + 1);
      q.symbol = cfg_.symbol;
      q.bidPrice = Price::fromRaw(midRaw_ - off);
      q.bidQty = Quantity::fromRaw(size);
      q.askPrice = Price::fromRaw(midRaw_ + off);
      q.askQty = Quantity::fromRaw(size);
      q.accountId = cfg_.account;
      q.lastLook = cfg_.lastLook;
      out.push_back(InboundCommand{q});
    }
    ++rounds_;
  }

  int64_t midRaw() const noexcept { return midRaw_; }
  uint64_t rounds() const noexcept { return rounds_; }
  // Commands per round: one Quote per level, each carrying both sides.
  int perRound() const noexcept { return cfg_.levels; }

 private:
  Config cfg_;
  Rng rng_;
  int64_t midRaw_;
  uint64_t rounds_{0};
};

}  // namespace flox::venue::script
