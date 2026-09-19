/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * How fast the scripted flow can be produced, and how fast a venue eats it.
 *
 * A generator that cannot outrun the engine it is driving is a bad harness:
 * the client under test would be measuring the generator. Two numbers, then:
 * what the generator costs on its own, and what the same flow costs once the
 * engine has matched it.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/script/quote_generators.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace flox;
using namespace flox::venue;
using namespace flox::venue::script;
using clk = std::chrono::steady_clock;

int main()
{
  constexpr int kRounds = 20000;

  LadderQuoter::Config cfg;
  cfg.symbol = 1;
  cfg.levels = 20;

  // Arm A: the generator alone. The buffer is cleared per round, exactly as
  // arm B does it -- the first version let it grow to 800k entries in arm A
  // only, so the two arms were measuring different amounts of allocation and
  // "generator + matching" came out FASTER than the generator, which is
  // nonsense on its face.
  {
    LadderQuoter q(cfg, 4242);
    std::vector<InboundCommand> out;
    out.reserve(static_cast<size_t>(q.perRound()));
    size_t total = 0;
    const auto t0 = clk::now();
    for (int i = 0; i < kRounds; ++i)
    {
      out.clear();
      q.next(out);
      total += out.size();
    }
    const auto t1 = clk::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("%-34s %12.0f commands/s (%zu commands)\n", "generator alone",
                static_cast<double>(total) / sec, total);
  }

  // Arm B: the same flow, matched.
  {
    venue::SymbolConfig sc;
    sc.id = 1;
    sc.tickSize = Price::fromRaw(1'000000);
    sc.minPrice = Price::fromRaw(1'00000000);
    sc.maxPrice = Price::fromRaw(1000'00000000);
    // Counted, not assumed: a generator whose commands the venue refuses
    // measures the rejection path and looks fast doing it.
    uint64_t events = 0;
    uint64_t rejects = 0;
    MatchingEngine<MatchingBook> eng(sc,
                                     [&](const OutboundEvent& e)
                                     {
                                       ++events;
                                       rejects += std::get_if<OrderRejected>(&e) != nullptr ? 1 : 0;
                                     });

    LadderQuoter q(cfg, 4242);
    std::vector<InboundCommand> out;
    out.reserve(static_cast<size_t>(q.perRound()));
    int64_t ts = 0;
    size_t total = 0;
    const auto t0 = clk::now();
    for (int i = 0; i < kRounds; ++i)
    {
      out.clear();
      q.next(out);
      for (const auto& c : out)
      {
        eng.submit(c, ++ts);
      }
      total += out.size();
    }
    const auto t1 = clk::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("%-34s %12.0f commands/s (%llu events out)\n", "generator + matching",
                static_cast<double>(total) / sec, static_cast<unsigned long long>(events));
    std::printf("%-34s %12llu\n", "  of which refused",
                static_cast<unsigned long long>(rejects));
  }
  return 0;
}
