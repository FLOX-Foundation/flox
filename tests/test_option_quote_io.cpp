#include <gtest/gtest.h>

#include <filesystem>

#include "flox/common.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

using namespace flox::replay;

namespace
{
OptionQuoteRecord makeQuote(int i, uint32_t sym = 7)
{
  OptionQuoteRecord q{};
  q.exchange_ts_ns = 1'000'000'000LL + i * 1'000'000LL;
  q.recv_ts_ns = q.exchange_ts_ns + 100;
  q.mark_price_raw = (3000LL + i) * 100000000LL;
  q.index_price_raw = (3001LL + i) * 100000000LL;
  q.iv_raw = static_cast<int64_t>((0.5 + i * 0.001) * kIvScale);
  q.open_interest_raw = (1000LL + i) * 100000000LL;
  q.underlying_price_raw = (3005LL + i) * 100000000LL;
  q.bid_price_raw = (2999LL + i) * 100000000LL;
  q.ask_price_raw = (3002LL + i) * 100000000LL;
  q.bid_size_raw = (10LL + i) * 100000000LL;
  q.ask_size_raw = (12LL + i) * 100000000LL;
  q.bid_iv_raw = static_cast<int64_t>((0.49 + i * 0.001) * kIvScale);
  q.ask_iv_raw = static_cast<int64_t>((0.51 + i * 0.001) * kIvScale);
  q.symbol_id = sym;
  q.instrument = static_cast<uint8_t>(flox::InstrumentType::Option);
  return q;
}
}  // namespace

class OptionQuoteIoTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() / "flox_test_option_quote_io";
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }
  std::filesystem::path _dir;
};

TEST_F(OptionQuoteIoTest, WriteAndReadUncompressed)
{
  {
    WriterConfig config{.output_dir = _dir};
    BinaryLogWriter writer(config);
    for (int i = 0; i < 100; ++i)
    {
      EXPECT_TRUE(writer.writeOptionQuote(makeQuote(i)));
    }
    writer.close();
    EXPECT_EQ(writer.stats().option_quotes_written, 100u);
  }

  ReaderConfig config{.data_dir = _dir};
  BinaryLogReader reader(config);
  int count = 0;
  reader.forEach(
      [&](const ReplayEvent& event)
      {
        EXPECT_EQ(event.type, EventType::OptionQuote);
        const auto expected = makeQuote(count);
        EXPECT_EQ(event.option_quote.exchange_ts_ns, expected.exchange_ts_ns);
        EXPECT_EQ(event.option_quote.mark_price_raw, expected.mark_price_raw);
        EXPECT_EQ(event.option_quote.iv_raw, expected.iv_raw);
        EXPECT_EQ(event.option_quote.open_interest_raw, expected.open_interest_raw);
        EXPECT_EQ(event.option_quote.underlying_price_raw, expected.underlying_price_raw);
        EXPECT_EQ(event.option_quote.bid_price_raw, expected.bid_price_raw);
        EXPECT_EQ(event.option_quote.ask_price_raw, expected.ask_price_raw);
        EXPECT_EQ(event.option_quote.bid_size_raw, expected.bid_size_raw);
        EXPECT_EQ(event.option_quote.ask_size_raw, expected.ask_size_raw);
        EXPECT_EQ(event.option_quote.bid_iv_raw, expected.bid_iv_raw);
        EXPECT_EQ(event.option_quote.ask_iv_raw, expected.ask_iv_raw);
        EXPECT_EQ(event.option_quote.symbol_id, 7u);
        EXPECT_EQ(event.symbolId(), 7u);  // helper resolves option-quote symbol
        ++count;
        return true;
      });
  EXPECT_EQ(count, 100);
}

TEST_F(OptionQuoteIoTest, InterleavedTradesAndQuotesInTimestampOrder)
{
  {
    WriterConfig config{.output_dir = _dir};
    BinaryLogWriter writer(config);
    for (int i = 0; i < 50; ++i)
    {
      TradeRecord t{};
      t.exchange_ts_ns = 1'000'000'000LL + (2 * i) * 1'000'000LL;
      t.price_raw = 100LL * 100000000LL;
      t.symbol_id = 7;
      EXPECT_TRUE(writer.writeTrade(t));

      auto q = makeQuote(0);
      q.exchange_ts_ns = 1'000'000'000LL + (2 * i + 1) * 1'000'000LL;
      EXPECT_TRUE(writer.writeOptionQuote(q));
    }
    writer.close();
  }

  ReaderConfig config{.data_dir = _dir};
  BinaryLogReader reader(config);
  int trades = 0, quotes = 0;
  int64_t last_ts = 0;
  reader.forEach(
      [&](const ReplayEvent& event)
      {
        EXPECT_GE(event.timestamp_ns, last_ts);  // non-decreasing
        last_ts = event.timestamp_ns;
        if (event.type == EventType::Trade)
        {
          ++trades;
        }
        else if (event.type == EventType::OptionQuote)
        {
          ++quotes;
        }
        return true;
      });
  EXPECT_EQ(trades, 50);
  EXPECT_EQ(quotes, 50);
}

TEST_F(OptionQuoteIoTest, CompressedRoundTrip)
{
  {
    WriterConfig config{.output_dir = _dir, .compression = CompressionType::LZ4};
    BinaryLogWriter writer(config);
    for (int i = 0; i < 100; ++i)
    {
      EXPECT_TRUE(writer.writeOptionQuote(makeQuote(i)));
    }
    writer.close();
    EXPECT_EQ(writer.stats().option_quotes_written, 100u);
  }

  ReaderConfig config{.data_dir = _dir};
  BinaryLogReader reader(config);
  int count = 0;
  reader.forEach(
      [&](const ReplayEvent& event)
      {
        EXPECT_EQ(event.type, EventType::OptionQuote);
        const auto expected = makeQuote(count);
        EXPECT_EQ(event.option_quote.iv_raw, expected.iv_raw);
        EXPECT_EQ(event.option_quote.mark_price_raw, expected.mark_price_raw);
        ++count;
        return true;
      });
  EXPECT_EQ(count, 100);
}
