/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * What a ladder update costs the JOURNAL.
 *
 * A maker holding N levels a side used to write one record per level, and a
 * shard whose journal is mostly quote traffic replays the whole quote stream
 * to reach one incident. The ladder's claim is that the same update is one
 * record, and that the record is SHORTER than the records it replaces -- not
 * merely fewer of them, which a fixed-width command carrying eight slots
 * would not have been.
 *
 * Measured, not computed: the bytes are taken off Journal::bytes() after the
 * records are actually written, which is the number an operator sees on
 * disk (framing and crc included), rather than a sum of sizeofs.
 *
 * The second column pair is the engine side, so a byte win bought with a
 * slower apply path would be visible here: nanoseconds to apply one update
 * through MatchingEngine::submit, ladder against the Quotes it stands for.
 *
 * Usage: bench_venue_quote_ladder [updates]
 */
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t ACCT = 7;
constexpr OrderId BID_BASE = 1000;
constexpr OrderId ASK_BASE = 2000;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = 0;
  c.quoteAsset = 1;
  return c;
}

QuoteLadder ladder(uint8_t levels, int nudge)
{
  QuoteLadder l;
  l.accountId = ACCT;
  l.symbol = SYM;
  l.bidIdBase = BID_BASE;
  l.askIdBase = ASK_BASE;
  l.levels = levels;
  for (uint8_t i = 0; i < levels; ++i)
  {
    l.level[i].bidPrice = px(99.99 - 0.01 * i + 0.01 * (nudge % 3));
    l.level[i].bidQty = qty(1.0 + i);
    l.level[i].askPrice = px(100.01 + 0.01 * i + 0.01 * (nudge % 3));
    l.level[i].askQty = qty(2.0 + i);
  }
  return l;
}

std::vector<Quote> asQuotes(const QuoteLadder& l)
{
  std::vector<Quote> v;
  for (uint8_t i = 0; i < quoteLadderLiveLevels(l); ++i)
  {
    Quote q;
    q.bidId = l.bidIdBase + i;
    q.askId = l.askIdBase + i;
    q.symbol = l.symbol;
    q.bidPrice = l.level[i].bidPrice;
    q.bidQty = l.level[i].bidQty;
    q.askPrice = l.level[i].askPrice;
    q.askQty = l.level[i].askQty;
    q.accountId = l.accountId;
    q.tif = l.tif;
    v.push_back(q);
  }
  return v;
}

// Bytes on disk for `updates` ladder updates of `levels` levels, written the
// way the shard writes them.
uint64_t ladderBytes(uint8_t levels, int updates, const std::string& path)
{
  std::filesystem::remove(path);
  Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
  for (int u = 0; u < updates; ++u)
  {
    j.append(InboundCommand{ladder(levels, u)}, u + 1);
  }
  j.flush();
  const uint64_t b = j.bytes();
  std::filesystem::remove(path);
  return b;
}

uint64_t quoteBytes(uint8_t levels, int updates, const std::string& path)
{
  std::filesystem::remove(path);
  Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
  int64_t ts = 1;
  for (int u = 0; u < updates; ++u)
  {
    for (const Quote& q : asQuotes(ladder(levels, u)))
    {
      j.append(InboundCommand{q}, ts++);
    }
  }
  j.flush();
  const uint64_t b = j.bytes();
  std::filesystem::remove(path);
  return b;
}

double ladderApplyNs(uint8_t levels, int updates)
{
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  const auto t0 = std::chrono::steady_clock::now();
  for (int u = 0; u < updates; ++u)
  {
    eng.submit(InboundCommand{ladder(levels, u)}, u + 1);
  }
  const auto t1 = std::chrono::steady_clock::now();
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / updates;
}

double quoteApplyNs(uint8_t levels, int updates)
{
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  int64_t ts = 1;
  const auto t0 = std::chrono::steady_clock::now();
  for (int u = 0; u < updates; ++u)
  {
    for (const Quote& q : asQuotes(ladder(levels, u)))
    {
      eng.submit(InboundCommand{q}, ts++);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / updates;
}

}  // namespace

int main(int argc, char** argv)
{
  const int updates = argc > 1 ? std::atoi(argv[1]) : 20000;
  const std::string path =
      (std::filesystem::temp_directory_path() / "flox_bench_quote_ladder.bin").string();

  std::printf("levels  journal-bytes/update            apply-ns/update\n");
  std::printf("        ladder  quotes  saved           ladder  quotes\n");
  for (uint8_t levels = 1; levels <= kQuoteLadderLevels; ++levels)
  {
    const double lb = double(ladderBytes(levels, updates, path)) / updates;
    const double qb = double(quoteBytes(levels, updates, path)) / updates;
    const double la = ladderApplyNs(levels, updates);
    const double qa = quoteApplyNs(levels, updates);
    std::printf("%6u  %6.0f  %6.0f  %4.2fx          %6.0f  %6.0f\n", unsigned(levels), lb, qb,
                qb / lb, la, qa);
  }
  return 0;
}
