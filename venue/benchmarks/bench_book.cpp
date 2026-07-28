/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Order-level book microbenchmark: the O(1) ladder against the map reference,
 * driven through the full engine.submit path (validation, risk gates, matching)
 * rather than a bare book call.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/workload.h"
#include "flox/book/ladder_book.h"
#include "flox/book/matching_book.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
constexpr SymbolId SYM = 1;

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  return c;
}

LadderBook::Config ladderCfg()
{
  return LadderBook::Config{/*base*/ 0, /*tick*/ Price::fromDouble(0.01).raw(),
                            /*levels*/ 16000, /*maxOrders*/ 1 << 20};
}

struct Result
{
  double nsPerOp{};
  double opsPerSec{};
  uint64_t trades{};
  uint64_t hash{};
};

template <class Book>
Result run(const std::vector<InboundCommand>& cmds, Book book)
{
  uint64_t trades = 0;
  uint64_t hash = 1469598103934665603ULL;
  auto sink = [&](const OutboundEvent& e)
  {
    if (std::get_if<Trade>(&e))
    {
      ++trades;
    }
    hash = hashEvent(hash, e);
  };
  MatchingEngine<Book> eng(cfg(), sink, std::move(book));

  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& c : cmds)
  {
    eng.submit(c);
  }
  const auto t1 = std::chrono::steady_clock::now();

  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  Result r;
  r.nsPerOp = ns / static_cast<double>(cmds.size());
  r.opsPerSec = static_cast<double>(cmds.size()) / (ns / 1e9);
  r.trades = trades;
  r.hash = hash;
  return r;
}
}  // namespace

int main()
{
  workload::Params p;
  p.symbol = SYM;
  p.count = 2'000'000;
  const auto cmds = workload::symmetricLimits(p);

  std::printf("workload: %zu commands, symmetric limits around %.2f (+/-%d ticks)\n\n", cmds.size(),
              p.mid, p.spreadTicks);

  const Result mapR = run<MatchingBook>(cmds, MatchingBook{});
  const Result ladR = run<LadderBook>(cmds, LadderBook{ladderCfg()});

  std::printf("%-16s  %10s  %14s  %10s\n", "book", "ns/op", "ops/s", "trades");
  std::printf("%-16s  %10.1f  %14.0f  %10llu\n", "map-reference", mapR.nsPerOp, mapR.opsPerSec,
              static_cast<unsigned long long>(mapR.trades));
  std::printf("%-16s  %10.1f  %14.0f  %10llu\n", "ladder-o1", ladR.nsPerOp, ladR.opsPerSec,
              static_cast<unsigned long long>(ladR.trades));

  const bool equal = (mapR.trades == ladR.trades) && (mapR.hash == ladR.hash);
  std::printf("\ndeterminism cross-check (map == ladder): %s\n", equal ? "PASS" : "FAIL");
  std::printf("ladder speedup: %.2fx\n", mapR.nsPerOp / ladR.nsPerOp);
  return equal ? 0 : 1;
}
