/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The C ABI entry contract: what every exported function promises a caller
// that holds handles as opaque integers and cannot catch a C++ exception.
//
//   1. A null handle returns the function's documented zero value.
//   2. No exception escapes the boundary.
//   3. Scratch buffers belong to a handle, not to a thread, and die with it.
//
// Each of these was reachable from a shipped binding before it was written
// down, so each gets a test rather than a comment.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::filesystem::path scratchDir(const char* leaf)
{
  auto dir = std::filesystem::temp_directory_path() / "flox_capi_contract" / leaf;
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

// ── 1. Null handles ───────────────────────────────────────────────────

// One call per handle family. Before the entry guard landed, sixteen of the
// twenty-one probes in the audit's sample died with SIGSEGV while their
// neighbours in the same layer quietly returned zero; the split was not
// documented anywhere, so a caller had no way to know which was which.
TEST(CapiContractTest, NullHandleReturnsZeroInsteadOfCrashing)
{
  EXPECT_EQ(flox_registry_symbol_count(nullptr), 0u);

  char exchange[32] = {0};
  char name[32] = {0};
  EXPECT_EQ(flox_registry_get_symbol_name(nullptr, 0, exchange, sizeof(exchange), name,
                                          sizeof(name)),
            0);

  uint32_t id = 12345;
  EXPECT_EQ(flox_registry_get_symbol_id(nullptr, "a", "b", &id), 0);

  EXPECT_EQ(flox_simulated_executor_fill_count(nullptr), 0u);
  flox_simulated_executor_submit_order(nullptr, 1, 0, 1.0, 1.0, 0, 1);
  flox_simulated_executor_cancel_all(nullptr, 1);
  flox_simulated_executor_on_trade(nullptr, 1, 100.0, 1);

  EXPECT_EQ(flox_account_id(nullptr), 0u);
  EXPECT_DOUBLE_EQ(flox_account_equity(nullptr), 0.0);
  flox_account_set_equity(nullptr, 1.0);

  flox_liquidation_engine_add_tier(nullptr, 1.0, 0.5);

  EXPECT_EQ(flox_venue_stack_account(nullptr), nullptr);
  EXPECT_EQ(flox_venue_stack_executor(nullptr), nullptr);
  EXPECT_EQ(flox_venue_stack_venue_name(nullptr), nullptr);

  EXPECT_EQ(flox_data_reader_count(nullptr), 0u);

  EXPECT_EQ(flox_book_get_bids(nullptr, nullptr, nullptr, 0), 0u);
}

// Destroy has always accepted null and must keep doing so: a binding's
// finaliser runs on a half-constructed object often enough that making this
// an error would be a downgrade.
TEST(CapiContractTest, DestroyStillAcceptsNull)
{
  flox_registry_destroy(nullptr);
  flox_account_destroy(nullptr);
  flox_simulated_executor_destroy(nullptr);
  flox_portfolio_risk_destroy(nullptr);
  flox_venue_stack_destroy(nullptr);
  flox_data_reader_destroy(nullptr);
  SUCCEED();
}

// A string argument is not a handle, so the entry guard does not cover it.
// This one path was a segmentation fault of its own.
TEST(CapiContractTest, DataReaderCreateRejectsNullPath)
{
  EXPECT_EQ(flox_data_reader_create(nullptr), nullptr);
}

TEST(CapiContractTest, SegmentPathArgumentsRejectNull)
{
  EXPECT_EQ(flox_segment_validate(nullptr), 0);
  EXPECT_EQ(flox_segment_merge(nullptr, "/tmp/out.flox"), 0);
  EXPECT_EQ(flox_segment_merge("/tmp", nullptr), 0);
}

TEST(CapiContractTest, SegmentMergeRejectsNullArguments)
{
  FloxMergeResult r = flox_segment_merge_full(nullptr, 2, "/tmp", "out", 1);
  EXPECT_EQ(r.success, 0);

  const char paths[] = "/tmp/a.flox\0/tmp/b.flox";
  r = flox_segment_merge_full(paths, 2, nullptr, "out", 1);
  EXPECT_EQ(r.success, 0);

  r = flox_segment_merge_full(paths, 2, "/tmp", nullptr, 1);
  EXPECT_EQ(r.success, 0);
}

// ── 2. Buffer arithmetic at the boundary ──────────────────────────────

// A caller that says "this buffer holds zero bytes" gets nothing written.
// The unsigned `exchange_len - 1` underflowed to SIZE_MAX, so strncpy's
// terminator landed at buf[SIZE_MAX], which wraps to buf[-1]; the function
// then reported success. Measured: 136 bytes zeroed from buf-65 to buf+70.
TEST(CapiContractTest, GetSymbolNameWithZeroLengthWritesNothing)
{
  FloxRegistryHandle reg = flox_registry_create();
  uint32_t sym = flox_registry_add_symbol(reg, "binance", "BTCUSDT", 0.01);

  constexpr size_t kArena = 512;
  constexpr size_t kMid = 256;
  std::vector<unsigned char> arena(kArena, 0xAA);
  char* buf = reinterpret_cast<char*>(arena.data()) + kMid;

  EXPECT_EQ(flox_registry_get_symbol_name(reg, sym, buf, 0, buf, 0), 0);

  for (size_t i = 0; i < kArena; ++i)
  {
    ASSERT_EQ(arena[i], 0xAA) << "byte " << i << " (offset " << (long)i - (long)kMid
                              << " from the buffer) was written through a zero-length buffer";
  }

  flox_registry_destroy(reg);
}

TEST(CapiContractTest, GetSymbolNameStillTruncatesIntoAShortBuffer)
{
  FloxRegistryHandle reg = flox_registry_create();
  uint32_t sym = flox_registry_add_symbol(reg, "binance", "BTCUSDT", 0.01);

  char exchange[4] = {0};
  char name[4] = {0};
  EXPECT_EQ(flox_registry_get_symbol_name(reg, sym, exchange, sizeof(exchange), name,
                                          sizeof(name)),
            1);
  EXPECT_STREQ(exchange, "bin");
  EXPECT_STREQ(name, "BTC");

  flox_registry_destroy(reg);
}

// ── 3. Scratch buffers belong to the handle ───────────────────────────

namespace
{

FloxPortfolioRiskRules dailyLossRules()
{
  FloxPortfolioRiskRules rules{};
  rules.has_max_daily_loss = 1;
  rules.max_daily_loss = -100.0;
  return rules;
}

void breachDailyLoss(FloxPortfolioRiskHandle h)
{
  FloxStrategyAccountFields f{};
  f.realized_pnl = -5000.0;
  flox_portfolio_risk_update(h, "alpha", &f, 0xFF);
}

}  // namespace

// The breach scratch used to outlive the handle it belonged to, in a
// thread-local map keyed by the raw pointer. malloc hands the same address
// back, so a freshly created, clean risk handle answered breach_at with the
// dead session's max_daily_loss breach and reported success.
TEST(CapiContractTest, BreachScratchDoesNotOutliveItsHandle)
{
  FloxPortfolioRiskRules rules = dailyLossRules();

  void* firstAddress = nullptr;
  for (int attempt = 0; attempt < 64; ++attempt)
  {
    FloxPortfolioRiskHandle dead = flox_portfolio_risk_create(&rules, 10000.0);
    ASSERT_NE(dead, nullptr);
    breachDailyLoss(dead);
    ASSERT_GT(flox_portfolio_risk_breach_count(dead), 0u);
    firstAddress = dead;
    flox_portfolio_risk_destroy(dead);

    FloxPortfolioRiskHandle fresh = flox_portfolio_risk_create(&rules, 10000.0);
    ASSERT_NE(fresh, nullptr);

    FloxBreach breach{};
    EXPECT_EQ(flox_portfolio_risk_breach_at(fresh, 0, &breach), 0)
        << "a clean handle served a breach from a destroyed one";
    EXPECT_EQ(flox_portfolio_risk_breach_count(fresh), 0u);

    const bool reused = (fresh == firstAddress);
    flox_portfolio_risk_destroy(fresh);
    if (reused)
    {
      SUCCEED() << "allocator reused the dead handle's address on attempt " << attempt;
      return;
    }
  }
  GTEST_SKIP() << "allocator never reused the address; the invariant still held every attempt";
}

// The count and the copy talked through a thread-local map, so splitting the
// pair across threads -- the one reason a C layer exists at all -- returned
// zero levels, which reads exactly like an empty book.
TEST(CapiContractTest, EncoderScratchCrossesThreads)
{
  FloxDeltaBookEncoderHandle enc = flox_delta_book_encoder_create(0);
  ASSERT_NE(enc, nullptr);

  FloxBookLevel bids[2] = {{100 * 100000000LL, 100000000LL}, {99 * 100000000LL, 200000000LL}};
  FloxBookLevel asks[2] = {{101 * 100000000LL, 100000000LL}, {102 * 100000000LL, 200000000LL}};

  uint8_t isDelta = 0;
  uint64_t bidCount = 0;
  uint64_t askCount = 0;
  flox_delta_book_encoder_encode(enc, 1, bids, 2, asks, 2, &isDelta, &bidCount, &askCount);
  ASSERT_EQ(bidCount, 2u);
  ASSERT_EQ(askCount, 2u);

  uint64_t workerBids = 0;
  uint64_t workerAsks = 0;
  FloxBookLevel outBids[4]{};
  FloxBookLevel outAsks[4]{};
  std::thread worker(
      [&]
      {
        workerBids = flox_delta_book_encoder_copy_bids(enc, outBids, 4);
        workerAsks = flox_delta_book_encoder_copy_asks(enc, outAsks, 4);
      });
  worker.join();

  EXPECT_EQ(workerBids, 2u) << "the encode result was invisible from another thread";
  EXPECT_EQ(workerAsks, 2u);
  EXPECT_EQ(outBids[0].price_raw, 100 * 100000000LL);

  flox_delta_book_encoder_destroy(enc);
}

TEST(CapiContractTest, BreachCountAndBreachAtCrossThreads)
{
  FloxPortfolioRiskRules rules = dailyLossRules();
  FloxPortfolioRiskHandle h = flox_portfolio_risk_create(&rules, 10000.0);
  ASSERT_NE(h, nullptr);
  breachDailyLoss(h);
  ASSERT_GT(flox_portfolio_risk_breach_count(h), 0u);

  uint8_t got = 0;
  FloxBreach breach{};
  std::thread worker([&]
                     { got = flox_portfolio_risk_breach_at(h, 0, &breach); });
  worker.join();

  EXPECT_EQ(got, 1) << "the breach list was invisible from another thread";
  EXPECT_STREQ(breach.rule, "max_daily_loss");

  flox_portfolio_risk_destroy(h);
}

// ── 4. Exceptions stop at the boundary ────────────────────────────────

// One unparsable timestamp in a user's CSV used to take down the whole host
// process: std::stoll throws, the frame has C linkage, and the runtime calls
// std::terminate. The shipped Node binding exposes this as
// BacktestRunner.runCsv, where JS cannot catch it either.
TEST(CapiContractTest, RunCsvWithAMalformedRowFailsInsteadOfTerminating)
{
  auto dir = scratchDir("run_csv");
  auto good = dir / "good.csv";
  auto bad = dir / "bad.csv";

  {
    std::ofstream f(good);
    f << "timestamp,open,high,low,close\n";
    f << "1700000000,100,101,99,100\n";
    f << "1700000060,100,101,99,101\n";
  }
  {
    std::ofstream f(bad);
    f << "timestamp,open,high,low,close\n";
    f << "1700000000,100,101,99,100\n";
    f << "NOT_A_NUMBER,100,101,99,101\n";
    f << "1700000120,100,101,99,102\n";
  }

  FloxRegistryHandle reg = flox_registry_create();
  flox_registry_add_symbol(reg, "sim", "BTCUSDT", 0.01);

  FloxBacktestRunnerHandle runner = flox_backtest_runner_create(reg, 0.0, 10000.0);
  ASSERT_NE(runner, nullptr);

  FloxBacktestStats stats{};
  EXPECT_EQ(flox_backtest_runner_run_csv(runner, good.string().c_str(), "BTCUSDT", &stats), 1);

  FloxBacktestStats badStats{};
  int rc = -1;
  try
  {
    rc = flox_backtest_runner_run_csv(runner, bad.string().c_str(), "BTCUSDT", &badStats);
  }
  catch (...)
  {
    ADD_FAILURE() << "an exception crossed the C ABI boundary; a C caller gets std::terminate";
  }
  EXPECT_EQ(rc, 0) << "a malformed row must fail the run, not abort the process";

  flox_backtest_runner_destroy(runner);
  flox_registry_destroy(reg);
  std::filesystem::remove_all(dir);
}

// The same class of escape, on a path with nothing to do with parsing:
// create_directories throws when the target cannot exist, and the writer
// constructor runs directly under C linkage.
TEST(CapiContractTest, DataWriterCreateOnAnImpossiblePathFailsInsteadOfTerminating)
{
  auto dir = scratchDir("writer");
  auto blocker = dir / "not_a_directory";
  {
    std::ofstream f(blocker);
    f << "x";
  }
  const std::string under = (blocker / "tape").string();

  FloxDataWriterHandle writer = nullptr;
  try
  {
    writer = flox_data_writer_create(under.c_str(), 0, 0);
  }
  catch (...)
  {
    ADD_FAILURE() << "an exception crossed the C ABI boundary; a C caller gets std::terminate";
  }
  EXPECT_EQ(writer, nullptr);
  flox_data_writer_destroy(writer);

  std::filesystem::remove_all(dir);
}
