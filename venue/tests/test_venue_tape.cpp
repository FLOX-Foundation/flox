/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/tape_recorder.h"
#include "flox-venue/workload.h"

#include "flox/replay/readers/binary_log_reader.h"

#include <gtest/gtest.h>
#include <cstdint>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
Price px(double v) { return Price::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

}  // namespace

TEST(Tape, EngineSuite)
{
  const std::string dir = "/tmp/flox_test_venue_tape_dir";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  uint64_t liveTrades = 0;
  __int128 liveNotional = 0;

  {
    TapeRecorder tape(dir, "tape.floxlog");
    MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                     {
                                       tape.onEvent(e);
                                       if (const auto* t = std::get_if<Trade>(&e))
                                       {
                                         ++liveTrades;
                                         liveNotional +=
                                             static_cast<__int128>(t->price.raw()) * t->quantity.raw();
                                       } });

    workload::Params p;
    p.symbol = SYM;
    p.count = 50'000;
    for (const auto& c : workload::symmetricLimits(p))
    {
      eng.submit(c);
    }
    CHECK(tape.tradesWritten() == liveTrades);
    tape.close();
  }

  // Read the floxlog tape back through flox's own reader.
  flox::replay::ReaderConfig rc;
  rc.data_dir = dir;
  rc.verify_crc = true;
  flox::replay::BinaryLogReader reader(rc);

  uint64_t tapeTrades = 0;
  __int128 tapeNotional = 0;
  reader.forEach([&](const flox::replay::ReplayEvent& e)
                 {
                   if (e.type == flox::replay::EventType::Trade)
                   {
                     ++tapeTrades;
                     tapeNotional += static_cast<__int128>(e.trade.price_raw) * e.trade.qty_raw;
                   }
                   return true; });

  std::printf("live trades=%llu, tape trades=%llu\n", static_cast<unsigned long long>(liveTrades),
              static_cast<unsigned long long>(tapeTrades));
  CHECK(tapeTrades == liveTrades);
  CHECK(tapeNotional == liveNotional);
  CHECK(liveTrades > 0);

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
