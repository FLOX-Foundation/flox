/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/binary_format_v1.h"
#include "flox/replay/ops/index_builder.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <benchmark/benchmark.h>
#include <filesystem>
#include <random>

using namespace flox::replay;

static std::filesystem::path g_test_dir;

static void SetupTestDir()
{
  if (g_test_dir.empty())
  {
    g_test_dir = std::filesystem::temp_directory_path() / "flox_bench_log";
    std::filesystem::remove_all(g_test_dir);
    std::filesystem::create_directories(g_test_dir);
  }
}

static void BM_WriteTrade(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "write_trade";
  std::filesystem::remove_all(dir);

  WriterConfig config{
      .output_dir = dir,
      .max_segment_bytes = 1ull << 30,  // 1GB - no rotation during bench
  };

  BinaryLogWriter writer(config);

  TradeRecord trade{};
  trade.exchange_ts_ns = 1000000000;
  trade.recv_ts_ns = 1000000100;
  trade.price_raw = 50000000000;
  trade.qty_raw = 1000000;
  trade.trade_id = 12345;
  trade.symbol_id = 1;
  trade.side = 1;

  for (auto _ : state)
  {
    writer.writeTrade(trade);
    ++trade.exchange_ts_ns;
    ++trade.trade_id;
  }

  writer.close();
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * (sizeof(FrameHeader) + sizeof(TradeRecord)));
}
BENCHMARK(BM_WriteTrade)->Unit(benchmark::kMicrosecond);

static void BM_WriteBook_10Levels(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "write_book_10";
  std::filesystem::remove_all(dir);

  WriterConfig config{
      .output_dir = dir,
      .max_segment_bytes = 1ull << 30,
  };

  BinaryLogWriter writer(config);

  BookRecordHeader header{};
  header.exchange_ts_ns = 1000000000;
  header.recv_ts_ns = 1000000100;
  header.seq = 1;
  header.symbol_id = 1;
  header.bid_count = 10;
  header.ask_count = 10;
  header.type = 1;  // delta

  std::vector<BookLevel> bids(10);
  std::vector<BookLevel> asks(10);
  for (int i = 0; i < 10; ++i)
  {
    bids[i] = {50000000000 - i * 1000000, 1000000 * (i + 1)};
    asks[i] = {50001000000 + i * 1000000, 1000000 * (i + 1)};
  }

  for (auto _ : state)
  {
    writer.writeBook(header, bids, asks);
    ++header.exchange_ts_ns;
    ++header.seq;
  }

  writer.close();
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() *
                          (sizeof(FrameHeader) + sizeof(BookRecordHeader) + 20 * sizeof(BookLevel)));
}
BENCHMARK(BM_WriteBook_10Levels)->Unit(benchmark::kMicrosecond);

static void BM_WriteBook_100Levels(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "write_book_100";
  std::filesystem::remove_all(dir);

  WriterConfig config{
      .output_dir = dir,
      .max_segment_bytes = 1ull << 30,
  };

  BinaryLogWriter writer(config);

  BookRecordHeader header{};
  header.exchange_ts_ns = 1000000000;
  header.recv_ts_ns = 1000000100;
  header.seq = 1;
  header.symbol_id = 1;
  header.bid_count = 100;
  header.ask_count = 100;
  header.type = 1;

  std::vector<BookLevel> bids(100);
  std::vector<BookLevel> asks(100);
  for (int i = 0; i < 100; ++i)
  {
    bids[i] = {50000000000 - i * 1000000, 1000000 * (i + 1)};
    asks[i] = {50001000000 + i * 1000000, 1000000 * (i + 1)};
  }

  for (auto _ : state)
  {
    writer.writeBook(header, bids, asks);
    ++header.exchange_ts_ns;
    ++header.seq;
  }

  writer.close();
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() *
                          (sizeof(FrameHeader) + sizeof(BookRecordHeader) + 200 * sizeof(BookLevel)));
}
BENCHMARK(BM_WriteBook_100Levels)->Unit(benchmark::kMicrosecond);

static void BM_ReadTrades(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "read_trades";
  std::filesystem::remove_all(dir);

  // Write test data
  const int kNumTrades = 100000;
  {
    WriterConfig config{.output_dir = dir};
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    for (int i = 0; i < kNumTrades; ++i)
    {
      trade.exchange_ts_ns = 1000000000 + i * 1000;
      trade.trade_id = i;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  for (auto _ : state)
  {
    ReaderConfig config{.data_dir = dir};
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent&)
                   {
      ++count;
      return true; });

    benchmark::DoNotOptimize(count);
  }

  state.SetItemsProcessed(state.iterations() * kNumTrades);
}
BENCHMARK(BM_ReadTrades)->Unit(benchmark::kMillisecond);

static void BM_ReadBooks_10Levels(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "read_books_10";
  std::filesystem::remove_all(dir);

  // Write test data
  const int kNumBooks = 50000;
  {
    WriterConfig config{.output_dir = dir};
    BinaryLogWriter writer(config);

    BookRecordHeader header{};
    header.bid_count = 10;
    header.ask_count = 10;

    std::vector<BookLevel> bids(10);
    std::vector<BookLevel> asks(10);
    for (int i = 0; i < 10; ++i)
    {
      bids[i] = {50000000000 - i * 1000000, 1000000};
      asks[i] = {50001000000 + i * 1000000, 1000000};
    }

    for (int i = 0; i < kNumBooks; ++i)
    {
      header.exchange_ts_ns = 1000000000 + i * 1000;
      header.seq = i;
      header.symbol_id = 1;
      writer.writeBook(header, bids, asks);
    }
    writer.close();
  }

  for (auto _ : state)
  {
    ReaderConfig config{.data_dir = dir};
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent&)
                   {
      ++count;
      return true; });

    benchmark::DoNotOptimize(count);
  }

  state.SetItemsProcessed(state.iterations() * kNumBooks);
}
BENCHMARK(BM_ReadBooks_10Levels)->Unit(benchmark::kMillisecond);

static void BM_CRC32(benchmark::State& state)
{
  const size_t size = state.range(0);
  std::vector<uint8_t> data(size);

  std::mt19937 rng(42);
  for (auto& b : data)
  {
    b = static_cast<uint8_t>(rng());
  }

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(Crc32::compute(data.data(), data.size()));
  }

  state.SetBytesProcessed(state.iterations() * size);
}
BENCHMARK(BM_CRC32)->Range(64, 64 << 10)->Unit(benchmark::kNanosecond);

static void BM_ReadWithTimeFilter(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "read_filter";

  // Write test data only once using static flag
  static bool data_written = false;
  const int kNumTrades = 100000;
  if (!data_written)
  {
    std::filesystem::remove_all(dir);
    WriterConfig config{
        .output_dir = dir,
        .max_segment_bytes = 1ull << 30,  // 1GB - no rotation
    };
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    for (int i = 0; i < kNumTrades; ++i)
    {
      trade.exchange_ts_ns = 1000000000LL + static_cast<int64_t>(i) * 100000LL;  // 100us apart
      trade.trade_id = i;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
    data_written = true;
  }

  // Filter to middle 1 second (10% of data)
  for (auto _ : state)
  {
    ReaderConfig config{
        .data_dir = dir,
        .from_ns = 5000000000,  // 5s
        .to_ns = 6000000000,    // 6s
    };
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent&)
                   {
      ++count;
      return true; });

    benchmark::DoNotOptimize(count);
  }

  state.SetItemsProcessed(state.iterations() * kNumTrades);
}
BENCHMARK(BM_ReadWithTimeFilter)->Unit(benchmark::kMillisecond);

static void BM_ReadWithSymbolFilter(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "read_symbol_filter";

  // Write test data only once using static flag
  static bool data_written = false;
  const int kNumTrades = 100000;
  if (!data_written)
  {
    std::filesystem::remove_all(dir);
    WriterConfig config{
        .output_dir = dir,
        .max_segment_bytes = 1ull << 30,  // 1GB - no rotation
    };
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    for (int i = 0; i < kNumTrades; ++i)
    {
      trade.exchange_ts_ns = 1000000000LL + static_cast<int64_t>(i) * 1000LL;
      trade.trade_id = i;
      trade.symbol_id = (i % 10) + 1;  // Symbols 1-10
      writer.writeTrade(trade);
    }
    writer.close();
    data_written = true;
  }

  // Filter to single symbol (10% of data)
  for (auto _ : state)
  {
    ReaderConfig config{
        .data_dir = dir,
        .symbols = {5},
    };
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent&)
                   {
      ++count;
      return true; });

    benchmark::DoNotOptimize(count);
  }

  state.SetItemsProcessed(state.iterations() * kNumTrades);
}
BENCHMARK(BM_ReadWithSymbolFilter)->Unit(benchmark::kMillisecond);

static void BM_SeekWithIndex(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "seek_with_index";
  std::filesystem::remove_all(dir);

  // Write 1M events with index
  const int kNumTrades = 1000000;
  {
    WriterConfig config{
        .output_dir = dir,
        .create_index = true,
        .index_interval = 1000,  // Index entry every 1000 events
    };
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    for (int i = 0; i < kNumTrades; ++i)
    {
      trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;  // 1ms apart
      trade.trade_id = i;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Seek to middle (500s mark) and read 1000 events
  const int64_t seek_target = 500000000000LL;  // 500s
  const int read_count = 1000;

  for (auto _ : state)
  {
    ReaderConfig config{.data_dir = dir};
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEachFrom(seek_target, [&](const ReplayEvent&)
                       {
      ++count;
      return count < read_count; });

    benchmark::DoNotOptimize(count);
  }

  state.SetItemsProcessed(state.iterations() * read_count);
}
BENCHMARK(BM_SeekWithIndex)->Unit(benchmark::kMicrosecond);

static void BM_SeekWithoutIndex(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "seek_without_index";
  std::filesystem::remove_all(dir);

  // Write 1M events WITHOUT index
  const int kNumTrades = 1000000;
  {
    WriterConfig config{
        .output_dir = dir,
        .create_index = false,
    };
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    for (int i = 0; i < kNumTrades; ++i)
    {
      trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      trade.trade_id = i;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Seek to middle and read 1000 events (will scan linearly)
  const int64_t seek_target = 500000000000LL;
  const int read_count = 1000;

  for (auto _ : state)
  {
    ReaderConfig config{.data_dir = dir};
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEachFrom(seek_target, [&](const ReplayEvent&)
                       {
      ++count;
      return count < read_count; });

    benchmark::DoNotOptimize(count);
  }

  state.SetItemsProcessed(state.iterations() * read_count);
}
BENCHMARK(BM_SeekWithoutIndex)->Unit(benchmark::kMillisecond);

static void BM_IndexBuild(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "index_build";

  // Write events without index (done once outside the loop)
  const int kNumTrades = static_cast<int>(state.range(0));
  std::filesystem::path segment_path;

  {
    std::filesystem::remove_all(dir);
    WriterConfig config{
        .output_dir = dir,
        .create_index = false,
    };
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    for (int i = 0; i < kNumTrades; ++i)
    {
      trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      trade.trade_id = i;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();

    // Find segment file
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
      if (entry.path().extension() == ".floxlog")
      {
        segment_path = entry.path();
        break;
      }
    }
  }

  for (auto _ : state)
  {
    state.PauseTiming();
    // Remove index before each iteration
    IndexBuilder::removeIndex(segment_path);
    state.ResumeTiming();

    IndexBuilderConfig builder_config{.index_interval = 1000};
    IndexBuilder builder(builder_config);
    auto result = builder.buildForSegment(segment_path);
    benchmark::DoNotOptimize(result);
  }

  state.SetItemsProcessed(state.iterations() * kNumTrades);
}
BENCHMARK(BM_IndexBuild)->Arg(10000)->Arg(100000)->Arg(1000000)->Unit(benchmark::kMillisecond);

static void BM_WriteWithIndex(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "write_with_index";
  std::filesystem::remove_all(dir);

  WriterConfig config{
      .output_dir = dir,
      .max_segment_bytes = 1ull << 30,
      .create_index = true,
      .index_interval = 1000,
  };

  BinaryLogWriter writer(config);

  TradeRecord trade{};
  trade.exchange_ts_ns = 1000000000;
  trade.price_raw = 50000000000;
  trade.qty_raw = 1000000;
  trade.symbol_id = 1;

  for (auto _ : state)
  {
    writer.writeTrade(trade);
    ++trade.exchange_ts_ns;
  }

  writer.close();
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteWithIndex)->Unit(benchmark::kMicrosecond);

static void BM_WriteWithoutIndex(benchmark::State& state)
{
  SetupTestDir();
  auto dir = g_test_dir / "write_without_index";
  std::filesystem::remove_all(dir);

  WriterConfig config{
      .output_dir = dir,
      .max_segment_bytes = 1ull << 30,
      .create_index = false,
  };

  BinaryLogWriter writer(config);

  TradeRecord trade{};
  trade.exchange_ts_ns = 1000000000;
  trade.price_raw = 50000000000;
  trade.qty_raw = 1000000;
  trade.symbol_id = 1;

  for (auto _ : state)
  {
    writer.writeTrade(trade);
    ++trade.exchange_ts_ns;
  }

  writer.close();
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteWithoutIndex)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
