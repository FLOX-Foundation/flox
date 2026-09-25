/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol test: Bitget ships both of the integrity fields its books
 * channel defines -- "seq" and "checksum" -- and the connector reads neither.
 * A dropped frame or a corrupted level therefore leaves a book that is wrong
 * and confident, with no counter, no log line and no health event; Bybit at
 * least detects its own gaps.
 *
 * Pinned checksum rule (Bitget books channel): take up to the first 25 levels
 * of each side, in the order the venue sent them, and join with ':'
 *
 *     bid[0].price:bid[0].size:ask[0].price:ask[0].size:bid[1].price:...
 *
 * using the venue's own price/size strings verbatim and stopping at whichever
 * side runs out. CRC32 (IEEE, the zlib polynomial) over that string,
 * reinterpreted as a signed 32-bit integer, is the value in "checksum".
 * Frames below carry the checksum of their own levels, so only snapshots are
 * used for the checksum cases -- a delta's checksum covers the merged book,
 * which is the connector's job to maintain, not this test's.
 *
 * No sockets: raw frames go straight into handleMessage.
 *
 * needs:
 *   public void BitgetExchangeConnector::handleMessage(std::string_view);
 *   uint64_t BitgetExchangeConnector::bookGapCount() const noexcept;
 *   uint64_t BitgetExchangeConnector::bookChecksumFailureCount() const noexcept;
 * -- handleMessage is private today, and neither counter exists. The gap and
 * checksum events themselves ride the existing SequenceGapCallback.
 */

#include "flox-connectors/bitget/bitget_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/log/abstract_logger.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace flox;

namespace
{

std::shared_ptr<AtomicLogger> makeLogger(const char* basename)
{
  auto dir = std::filesystem::temp_directory_path() / "flox_bitget_integrity_test_logs";
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = basename;
  return std::make_shared<AtomicLogger>(opts);
}

// Keeps every line the connector logs. resubscribeBook() is a private method
// with no counter of its own, and the socket it would write to is not
// connected in an offline test -- but the connector treats a failed
// resubscribe send as a reportable condition and logs it, which is the one
// observable trace that the resync was actually asked for.
class RecordingLogger final : public ILogger
{
 public:
  void info(std::string_view) override {}
  void warn(std::string_view line) override { record(line); }
  void error(std::string_view line) override { record(line); }

  size_t countContaining(std::string_view needle)
  {
    std::lock_guard<std::mutex> lk(_m);
    size_t n = 0;
    for (const auto& line : _lines)
    {
      n += line.find(needle) != std::string::npos ? 1u : 0u;
    }
    return n;
  }

 private:
  void record(std::string_view line)
  {
    std::lock_guard<std::mutex> lk(_m);
    _lines.emplace_back(line);
  }

  std::mutex _m;
  std::vector<std::string> _lines;
};

uint32_t crc32(std::string_view s)
{
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char c : s)
  {
    crc ^= c;
    for (int k = 0; k < 8; ++k)
    {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

struct Level
{
  std::string price;
  std::string size;
};

int32_t bitgetChecksum(const std::vector<Level>& bids, const std::vector<Level>& asks)
{
  std::string joined;
  const size_t n = std::min<size_t>(25, std::max(bids.size(), asks.size()));
  for (size_t i = 0; i < n; ++i)
  {
    if (i < bids.size())
    {
      if (!joined.empty())
      {
        joined += ':';
      }
      joined += bids[i].price + ':' + bids[i].size;
    }
    if (i < asks.size())
    {
      if (!joined.empty())
      {
        joined += ':';
      }
      joined += asks[i].price + ':' + asks[i].size;
    }
  }
  return static_cast<int32_t>(crc32(joined));
}

std::string levelsJson(const std::vector<Level>& ls)
{
  std::string s = "[";
  for (size_t i = 0; i < ls.size(); ++i)
  {
    if (i)
    {
      s += ',';
    }
    s += "[\"" + ls[i].price + "\",\"" + ls[i].size + "\"]";
  }
  s += ']';
  return s;
}

// A books15 frame. checksum is emitted only when withChecksum is set, so the
// sequence cases below are independent of the checksum rule.
std::string bookFrame(const char* action, int64_t seq, const std::vector<Level>& bids,
                      const std::vector<Level>& asks, bool withChecksum, int32_t checksum)
{
  std::string s =
      R"({"action":")" + std::string(action) +
      R"(","arg":{"instType":"USDT-FUTURES","channel":"books15","instId":"BTCUSDT"},"data":[{)";
  s += R"("asks":)" + levelsJson(asks);
  s += R"(,"bids":)" + levelsJson(bids);
  if (withChecksum)
  {
    s += R"(,"checksum":)" + std::to_string(checksum);
  }
  s += R"(,"seq":)" + std::to_string(seq);
  s += R"(,"ts":"1700000000000"}],"ts":1700000000000})";
  return s;
}

class CountingSub final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 41; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _bestBids.push_back(ev.update.bids.empty() ? 0.0 : ev.update.bids[0].price.toDouble());
  }

  std::vector<double> bestBids()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _bestBids;
  }

 private:
  std::mutex _m;
  std::vector<double> _bestBids;
};

struct Harness
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  CountingSub sub;
  SymbolRegistry registry;
  std::unique_ptr<BitgetExchangeConnector> connector;

  std::mutex m;
  std::vector<std::pair<uint64_t, uint64_t>> gaps;

  explicit Harness(const char* logName, std::shared_ptr<ILogger> injected = nullptr)
  {
    bookBus.subscribe(&sub);
    bookBus.start();
    tradeBus.start();

    SymbolInfo btc{};
    btc.symbol = "BTCUSDT";
    btc.exchange = "bitget";
    btc.type = InstrumentType::Future;
    registry.registerSymbol(btc);

    BitgetConfig cfg;
    cfg.publicEndpoint = "wss://unused.invalid";
    cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};

    connector = std::make_unique<BitgetExchangeConnector>(
        cfg, &bookBus, &tradeBus, nullptr, &registry,
        injected ? std::move(injected) : std::static_pointer_cast<ILogger>(makeLogger(logName)));
    connector->setErrorCallbacks(
        [](std::string_view)
        {
        },
        [this](uint64_t expected, uint64_t received)
        {
          std::lock_guard<std::mutex> lk(m);
          gaps.emplace_back(expected, received);
        },
        [](SymbolId, uint64_t)
        {
        });
  }

  ~Harness()
  {
    bookBus.flush();
    bookBus.stop();
    tradeBus.stop();
  }

  size_t gapCount()
  {
    std::lock_guard<std::mutex> lk(m);
    return gaps.size();
  }

  std::vector<std::pair<uint64_t, uint64_t>> seenGaps()
  {
    std::lock_guard<std::mutex> lk(m);
    return gaps;
  }
};

const std::vector<Level> kBids{{"100.5", "1.5"}, {"100.4", "2"}};
const std::vector<Level> kAsks{{"100.6", "1"}, {"100.7", "3"}};

// A book deeper than 20 levels a side. The checksum rule covers the first 25,
// so every level here is inside it and no two prefixes of it hash alike: a
// checksum taken over a different number of levels does not match.
std::vector<Level> deepLevels(int firstCents, int step)
{
  std::vector<Level> levels;
  for (int i = 0; i < 25; ++i)
  {
    const int cents = firstCents + i * step;
    levels.push_back({std::to_string(cents / 100) + "." + (cents % 100 < 10 ? "0" : "") +
                          std::to_string(cents % 100),
                      std::to_string(i + 1)});
  }
  return levels;
}

}  // namespace

// Control (green once the checksum is read at all): a snapshot whose checksum
// matches its levels is published untouched and counts no failure.
TEST(BitgetBookIntegrity, MatchingChecksumIsPublished)
{
  Harness h("bitget_checksum_ok.log");

  h.connector->handleMessage(
      bookFrame("snapshot", 1, kBids, kAsks, true, bitgetChecksum(kBids, kAsks)));
  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookChecksumFailureCount(), 0u);
  const auto bids = h.sub.bestBids();
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_DOUBLE_EQ(bids[0], 100.5);
}

// The finding: the checksum is ignored, so a corrupted book is published as
// if it were sound.
TEST(BitgetBookIntegrity, MismatchedChecksumIsNotPublished)
{
  Harness h("bitget_checksum_bad.log");

  const int32_t wrong = bitgetChecksum(kBids, kAsks) ^ 0x5A5A5A5A;
  h.connector->handleMessage(bookFrame("snapshot", 1, kBids, kAsks, true, wrong));
  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookChecksumFailureCount(), 1u)
      << "a checksum mismatch must be detected and counted";
  EXPECT_TRUE(h.sub.bestBids().empty())
      << "a book that failed its own checksum must not be published";
  EXPECT_GE(h.gapCount(), 1u) << "an invalid book is a feed-health event, not a silent drop";
}

// A book invalidated by a bad checksum stays invalid until a fresh snapshot
// re-baselines it: deltas arriving in between would apply onto a book the
// connector already knows is wrong.
TEST(BitgetBookIntegrity, InvalidBookSuppressesDeltasUntilAFreshSnapshot)
{
  Harness h("bitget_checksum_resync.log");

  const int32_t wrong = bitgetChecksum(kBids, kAsks) ^ 0x5A5A5A5A;
  h.connector->handleMessage(bookFrame("snapshot", 1, kBids, kAsks, true, wrong));
  h.connector->handleMessage(bookFrame("update", 2, {{"100.55", "1"}}, {}, false, 0));
  h.bookBus.flush();
  EXPECT_TRUE(h.sub.bestBids().empty());

  h.connector->handleMessage(
      bookFrame("snapshot", 3, kBids, kAsks, true, bitgetChecksum(kBids, kAsks)));
  h.bookBus.flush();

  const auto bids = h.sub.bestBids();
  ASSERT_EQ(bids.size(), 1u) << "a fresh, valid snapshot must re-baseline the book";
  EXPECT_DOUBLE_EQ(bids[0], 100.5);
}

// The finding: "seq" is never read, so a dropped frame leaves the local book
// permanently behind with nothing to say so.
TEST(BitgetBookIntegrity, SequenceGapIsDetectedAndReported)
{
  Harness h("bitget_seq_gap.log");

  h.connector->handleMessage(bookFrame("snapshot", 10, kBids, kAsks, false, 0));
  h.connector->handleMessage(bookFrame("update", 11, {{"100.55", "1"}}, {}, false, 0));
  h.connector->handleMessage(bookFrame("update", 15, {{"100.56", "1"}}, {}, false, 0));
  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookGapCount(), 1u) << "seq 12..14 were dropped by the venue";
  const auto gaps = h.seenGaps();
  ASSERT_EQ(gaps.size(), 1u) << "a book sequence gap must reach emitSequenceGap";
  EXPECT_EQ(gaps[0].first, 12u) << "the expected seq is the one after the last good frame";
  EXPECT_EQ(gaps[0].second, 15u) << "the received seq is the one that broke continuity";

  const auto bids = h.sub.bestBids();
  ASSERT_EQ(bids.size(), 2u) << "the delta after the gap must not be applied";
}

// A book invalidated by a bad checksum must stay invalid even when it had been
// valid a frame earlier. Without that the deltas that follow are applied on
// top of levels the venue has already said are wrong -- and the earlier valid
// snapshot is what makes the difference observable, because a connector whose
// books start out invalid suppresses them either way.
TEST(BitgetBookIntegrity, ASoundBookInvalidatedByAChecksumStopsPublishingDeltas)
{
  Harness h("bitget_checksum_invalidates.log");

  h.connector->handleMessage(
      bookFrame("snapshot", 1, kBids, kAsks, true, bitgetChecksum(kBids, kAsks)));
  h.bookBus.flush();
  ASSERT_EQ(h.sub.bestBids().size(), 1u) << "the sound snapshot must publish";

  const int32_t wrong = bitgetChecksum(kBids, kAsks) ^ 0x5A5A5A5A;
  h.connector->handleMessage(bookFrame("snapshot", 2, kBids, kAsks, true, wrong));
  h.connector->handleMessage(bookFrame("update", 3, {{"100.55", "1"}}, {}, false, 0));
  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookChecksumFailureCount(), 1u);
  EXPECT_EQ(h.sub.bestBids().size(), 1u)
      << "the delta after a failed checksum must not be published: the book it would extend is "
         "the one the venue said is wrong";
}

// The checksum covers the first 25 levels of each side, not the first 20. A
// book deeper than 20 levels hashes differently under the two rules, so a
// connector counting 20 fails a correct checksum on every books-channel frame
// and resyncs forever.
TEST(BitgetBookIntegrity, ChecksumCoversTwentyFiveLevelsPerSide)
{
  Harness h("bitget_checksum_depth.log");

  const std::vector<Level> bids = deepLevels(10050, -1);
  const std::vector<Level> asks = deepLevels(10060, 1);
  ASSERT_GT(bids.size(), 20u) << "the fixture has to reach past the 20th level to tell the "
                                 "two rules apart";

  h.connector->handleMessage(
      bookFrame("snapshot", 1, bids, asks, true, bitgetChecksum(bids, asks)));
  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookChecksumFailureCount(), 0u)
      << "a correct 25-level checksum on a deep book must verify";
  ASSERT_EQ(h.sub.bestBids().size(), 1u) << "a sound deep book must be published";
  EXPECT_DOUBLE_EQ(h.sub.bestBids()[0], 100.5);
}

// A failed checksum must also ask for a fresh snapshot: invalidating the book
// without re-subscribing leaves the symbol dark until the next full reconnect.
// The socket is not connected in an offline test, so the resubscribe send
// fails -- and the connector reports that failure, which is the trace this
// checks for.
TEST(BitgetBookIntegrity, AChecksumFailureAsksForAFreshSnapshot)
{
  auto logger = std::make_shared<RecordingLogger>();
  Harness h("bitget_checksum_resubscribe.log", logger);

  const int32_t wrong = bitgetChecksum(kBids, kAsks) ^ 0x5A5A5A5A;
  h.connector->handleMessage(bookFrame("snapshot", 1, kBids, kAsks, true, wrong));
  h.bookBus.flush();

  ASSERT_EQ(h.connector->bookChecksumFailureCount(), 1u);
  EXPECT_EQ(logger->countContaining("resubscribe send failed for BTCUSDT"), 1u)
      << "an invalidated book must be re-subscribed, or no snapshot ever comes back";
}

// A delta's checksum covers the merged book, which this connector never
// builds -- it forwards frames. Failing a delta against it would invalidate a
// healthy book and force a resync loop on every frame the venue stamps, so a
// delta is checked on "seq" alone.
TEST(BitgetBookIntegrity, ADeltaIsNotFailedAgainstItsOwnChecksum)
{
  Harness h("bitget_checksum_delta.log");

  h.connector->handleMessage(
      bookFrame("snapshot", 1, kBids, kAsks, true, bitgetChecksum(kBids, kAsks)));
  // A checksum over the merged book: it cannot match this frame's own two
  // levels, and it must not be treated as if it should.
  h.connector->handleMessage(bookFrame("update", 2, {{"100.55", "1"}}, {}, true, 12345));
  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookChecksumFailureCount(), 0u)
      << "a delta must not be failed against a checksum of a book the connector does not keep";
  const auto bids = h.sub.bestBids();
  ASSERT_EQ(bids.size(), 2u) << "the delta must still be published";
  EXPECT_DOUBLE_EQ(bids[1], 100.55);
}

// Control (green today): contiguous frames are published in order and count
// no gap.
TEST(BitgetBookIntegrity, ContiguousSequenceIsPublished)
{
  Harness h("bitget_seq_ok.log");

  h.connector->handleMessage(bookFrame("snapshot", 10, kBids, kAsks, false, 0));
  h.connector->handleMessage(bookFrame("update", 11, {{"100.55", "1"}}, {}, false, 0));
  h.connector->handleMessage(bookFrame("update", 12, {{"100.56", "1"}}, {}, false, 0));
  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookGapCount(), 0u);
  EXPECT_EQ(h.gapCount(), 0u);
  EXPECT_EQ(h.sub.bestBids().size(), 3u);
}
