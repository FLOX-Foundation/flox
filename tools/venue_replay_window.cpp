/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * "What happened to account N between t1 and t2."
 *
 * Replays a venue snapshot plus its journal segments and prints the events
 * inside a window, with the sequencer timestamps they were produced under and
 * a digest over the whole replayed stream. The digest is the part that makes
 * the extract evidence rather than a story: compared against the live run's,
 * equal digests say this replay took the path the venue took.
 *
 * The replay itself is in flox-venue/journal_window.h and is tested there.
 * This file is argument parsing and printing, deliberately.
 */
#include "flox-venue/journal_window.h"
#include "flox-venue/matching_book.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

void usage(const char* argv0)
{
  std::fprintf(stderr,
               "usage: %s --symbol N --from NS --to NS [--account N] [--snapshot FILE]\n"
               "          [--tick RAW --min-price RAW --max-price RAW] SEGMENT...\n"
               "\n"
               "  --from/--to   sequencer nanoseconds, both ends included\n"
               "  --account     0 or omitted = every account\n"
               "  --snapshot    applied before the segments, as recovery does\n"
               "\n"
               "The instrument parameters must match the engine that WROTE the\n"
               "journal: raw fixed-point state restored under other scales is\n"
               "silently reinterpreted, which is why they are asked for rather\n"
               "than guessed.\n",
               argv0);
}

int64_t asI64(const char* s) { return std::strtoll(s, nullptr, 10); }
uint64_t asU64(const char* s) { return std::strtoull(s, nullptr, 10); }

}  // namespace

int main(int argc, char** argv)
{
  SymbolConfig cfg;
  std::string snapshot;
  std::vector<std::string> segments;
  WindowQuery q;
  bool haveSymbol = false;
  bool haveFrom = false;
  bool haveTo = false;
  int64_t tickRaw = 0;
  int64_t minPriceRaw = 0;
  int64_t maxPriceRaw = 0;

  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> const char*
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--symbol")
    {
      cfg.id = static_cast<SymbolId>(asU64(next("--symbol")));
      haveSymbol = true;
    }
    else if (a == "--from")
    {
      q.fromNs = asI64(next("--from"));
      haveFrom = true;
    }
    else if (a == "--to")
    {
      q.toNs = asI64(next("--to"));
      haveTo = true;
    }
    else if (a == "--account")
    {
      q.account = asU64(next("--account"));
    }
    else if (a == "--snapshot")
    {
      snapshot = next("--snapshot");
    }
    else if (a == "--tick")
    {
      tickRaw = asI64(next("--tick"));
    }
    else if (a == "--min-price")
    {
      minPriceRaw = asI64(next("--min-price"));
    }
    else if (a == "--max-price")
    {
      maxPriceRaw = asI64(next("--max-price"));
    }
    else if (a == "-h" || a == "--help")
    {
      usage(argv[0]);
      return 0;
    }
    else if (!a.empty() && a[0] == '-')
    {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      usage(argv[0]);
      return 2;
    }
    else
    {
      segments.push_back(a);
    }
  }

  if (!haveSymbol || !haveFrom || !haveTo || segments.empty())
  {
    usage(argv[0]);
    return 2;
  }
  if (q.fromNs > q.toNs)
  {
    std::fprintf(stderr, "--from is after --to\n");
    return 2;
  }
  cfg.tickSize = Price::fromRaw(tickRaw);
  cfg.minPrice = Price::fromRaw(minPriceRaw);
  cfg.maxPrice = Price::fromRaw(maxPriceRaw);

  WindowResult r;
  try
  {
    r = replayWindow<MatchingBook>(cfg, snapshot, segments, q);
  }
  catch (const JournalFormatError& e)
  {
    // Not softened into a partial answer. A short history that looks complete
    // is the one outcome a dispute cannot survive.
    std::fprintf(stderr, "refused: %s\n", e.what());
    return 3;
  }

  std::printf("# records replayed: %llu\n", static_cast<unsigned long long>(r.recordsReplayed));
  std::printf("# events total: %llu, in window: %llu, after account filter: %zu\n",
              static_cast<unsigned long long>(r.eventsTotal),
              static_cast<unsigned long long>(r.eventsInWindow), r.events.size());
  std::printf("# stream digest: %016llx\n", static_cast<unsigned long long>(r.streamDigest));
  for (const WindowEvent& we : r.events)
  {
    std::printf("%lld\t%zu\t%llu\n", static_cast<long long>(we.ts), we.event.index(),
                static_cast<unsigned long long>(accountOf(we.event)));
  }
  return 0;
}
