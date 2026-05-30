#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "flox/replay/binary_format_v1.h"

using namespace flox::replay;

TEST(OptionQuoteRecord, SizeAndAlignment)
{
  static_assert(sizeof(OptionQuoteRecord) == 72);
  static_assert(alignof(OptionQuoteRecord) == 8);
  EXPECT_EQ(sizeof(OptionQuoteRecord) % 8, 0u);
}

TEST(OptionQuoteRecord, EventTypeValue)
{
  EXPECT_EQ(static_cast<uint8_t>(EventType::OptionQuote), 4);
}

TEST(OptionQuoteRecord, IvScaleRoundTrip)
{
  const double iv = 0.6532;
  const int64_t raw = static_cast<int64_t>(iv * kIvScale);
  const double back = static_cast<double>(raw) / kIvScale;
  EXPECT_NEAR(back, iv, 1.0 / kIvScale);
}

// Mirror the reader's unknown-frame skip logic (MmapSegmentReader::next) on a
// hand-built buffer: an OptionQuote frame followed by a Trade frame. A consumer
// that only understands Trade must skip the OptionQuote via FrameHeader.size and
// still land on the Trade. This validates the additive/backward-compatible
// design before the real writer/reader path is wired (W16.T012).
TEST(OptionQuoteRecord, UnknownFrameSkippedByTradeOnlyConsumer)
{
  std::vector<std::byte> buf;
  auto append = [&](const void* p, size_t n)
  {
    const auto* b = static_cast<const std::byte*>(p);
    buf.insert(buf.end(), b, b + n);
  };

  // Frame 1: OptionQuote
  OptionQuoteRecord oq{};
  oq.exchange_ts_ns = 1000;
  oq.iv_raw = static_cast<int64_t>(0.5 * kIvScale);
  oq.symbol_id = 7;
  FrameHeader fh1{};
  fh1.size = sizeof(OptionQuoteRecord);
  fh1.type = static_cast<uint8_t>(EventType::OptionQuote);
  append(&fh1, sizeof(fh1));
  append(&oq, sizeof(oq));

  // Frame 2: Trade
  TradeRecord tr{};
  tr.exchange_ts_ns = 2000;
  tr.price_raw = 12345;
  tr.symbol_id = 7;
  FrameHeader fh2{};
  fh2.size = sizeof(TradeRecord);
  fh2.type = static_cast<uint8_t>(EventType::Trade);
  append(&fh2, sizeof(fh2));
  append(&tr, sizeof(tr));

  // Walk frames as a Trade-only consumer.
  size_t pos = 0;
  bool foundTrade = false;
  while (pos + sizeof(FrameHeader) <= buf.size())
  {
    FrameHeader hdr;
    std::memcpy(&hdr, buf.data() + pos, sizeof(FrameHeader));
    const size_t frameSize = sizeof(FrameHeader) + hdr.size;
    ASSERT_LE(pos + frameSize, buf.size());
    if (hdr.type == static_cast<uint8_t>(EventType::Trade))
    {
      TradeRecord got;
      std::memcpy(&got, buf.data() + pos + sizeof(FrameHeader), sizeof(TradeRecord));
      EXPECT_EQ(got.exchange_ts_ns, 2000);
      EXPECT_EQ(got.price_raw, 12345);
      foundTrade = true;
    }
    // Unknown types (OptionQuote here) fall through and advance by frameSize.
    pos += frameSize;
  }
  EXPECT_TRUE(foundTrade);
}
