/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// Counterexample minimisation for the venue fuzzes.
//
// A fuzz that reports "diverged at command 47,213" has found a real defect and
// handed over a report nobody can act on: the 47,213 commands before it are
// almost all irrelevant, and finding out which few matter is manual work. The
// command stream is deterministic and replayable, so the harness can answer
// "does this shorter stream still fail?" itself, and keep asking until no
// single remaining command can be dropped.
//
// This is delta debugging, run only after a failure -- the green path pays for
// recording the stream and nothing else. Two phases:
//
//   1. Shortest failing prefix, by binary search. A stream that fails at
//      command N usually fails on some prefix of it, and the search costs
//      log2(N) replays rather than N.
//   2. Chunk removal, halving the chunk size each round. Removing a command
//      can only make later commands referentially stale (a cancel for an id
//      that was never created), which the engine rejects deterministically --
//      so a shorter stream is always a legal stream.
//
// The predicate must be a pure replay: build fresh engines, run the commands,
// return whether the failure reproduced. Anything it carries over between
// calls makes the result meaningless.

#include "flox-venue/messages.h"

#include <cstddef>
#include <cstdio>
#include <vector>

namespace flox::venue::fuzz
{

struct ShrinkLimits
{
  // Replays are the expensive unit; a deep fuzz stream makes each one cost
  // real time. The cap keeps a failing CI run from turning into a timeout,
  // and a partly-shrunk counterexample is still far better than the full one.
  int maxReplays{400};
};

struct ShrinkReport
{
  std::vector<InboundCommand> commands;
  int replays{0};
  bool hitLimit{false};
};

// `stillFails(first, count)` replays commands [first, first+count) of the
// candidate vector and returns true when the failure reproduces.
template <class Fails>
ShrinkReport shrinkCounterexample(const std::vector<InboundCommand>& full, Fails stillFails,
                                  ShrinkLimits limits = {})
{
  ShrinkReport out;
  out.commands = full;

  auto fails = [&](const std::vector<InboundCommand>& candidate)
  {
    ++out.replays;
    return stillFails(candidate);
  };

  if (full.empty())
  {
    return out;
  }

  // Phase 1: shortest failing prefix.
  {
    size_t lo = 1, hi = out.commands.size();
    while (lo < hi && out.replays < limits.maxReplays)
    {
      const size_t mid = lo + (hi - lo) / 2;
      std::vector<InboundCommand> prefix(out.commands.begin(),
                                         out.commands.begin() + static_cast<long>(mid));
      if (fails(prefix))
      {
        hi = mid;
      }
      else
      {
        lo = mid + 1;
      }
    }
    if (hi < out.commands.size())
    {
      out.commands.resize(hi);
    }
  }

  // Phase 2: drop chunks, halving until single commands.
  size_t chunk = out.commands.size() / 2;
  while (chunk >= 1 && out.replays < limits.maxReplays)
  {
    bool droppedAny = false;
    size_t at = 0;
    while (at < out.commands.size() && out.replays < limits.maxReplays)
    {
      const size_t take = (chunk < out.commands.size() - at) ? chunk : (out.commands.size() - at);
      std::vector<InboundCommand> candidate;
      candidate.reserve(out.commands.size() - take);
      candidate.insert(candidate.end(), out.commands.begin(),
                       out.commands.begin() + static_cast<long>(at));
      candidate.insert(candidate.end(), out.commands.begin() + static_cast<long>(at + take),
                       out.commands.end());
      if (!candidate.empty() && fails(candidate))
      {
        out.commands = std::move(candidate);
        droppedAny = true;
        // `at` stays: the next chunk slid into this position.
      }
      else
      {
        at += take;
      }
    }
    if (!droppedAny)
    {
      chunk /= 2;
    }
  }

  out.hitLimit = out.replays >= limits.maxReplays;
  return out;
}

// One line per command, enough to paste into a regression test.
inline void printCounterexample(const ShrinkReport& r)
{
  std::printf("\n--- minimal counterexample: %zu command(s), %d replay(s)%s ---\n",
              r.commands.size(), r.replays, r.hitLimit ? " (replay cap reached)" : "");
  for (size_t i = 0; i < r.commands.size(); ++i)
  {
    const InboundCommand& c = r.commands[i];
    if (const auto* o = std::get_if<NewOrder>(&c))
    {
      std::printf("  [%zu] NewOrder id=%llu acct=%llu %s type=%d px=%lld qty=%lld tif=%d stp=%d\n", i,
                  static_cast<unsigned long long>(o->id),
                  static_cast<unsigned long long>(o->accountId),
                  o->side == Side::BUY ? "BUY" : "SELL", static_cast<int>(o->type),
                  static_cast<long long>(o->price.raw()), static_cast<long long>(o->quantity.raw()),
                  static_cast<int>(o->tif), static_cast<int>(o->stp));
    }
    else if (const auto* x = std::get_if<CancelOrder>(&c))
    {
      std::printf("  [%zu] CancelOrder id=%llu\n", i, static_cast<unsigned long long>(x->id));
    }
    else if (const auto* m = std::get_if<ModifyOrder>(&c))
    {
      std::printf("  [%zu] ModifyOrder id=%llu px=%lld qty=%lld\n", i,
                  static_cast<unsigned long long>(m->id),
                  static_cast<long long>(m->newPrice.raw()),
                  static_cast<long long>(m->newQty.raw()));
    }
    else
    {
      std::printf("  [%zu] command variant %zu\n", i, c.index());
    }
  }
  std::printf("--- end counterexample ---\n\n");
}

}  // namespace flox::venue::fuzz
