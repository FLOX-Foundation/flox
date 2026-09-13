/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol test (CONN-13): Polymarket book/price_change/trade
 * messages carry the venue's own "timestamp" field, but the connector used
 * to stamp exchangeTsNs with local processing time instead of reading it.
 * Feed frames with a timestamp far in the past and assert the event
 * reports that timestamp, not "now".
 */

#include "flox-connectors/polymarket/polymarket_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/log/atomic_logger.h>
#include <flox/util/base/time.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace flox;

namespace
{

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_poly_timestamp_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

class CapturingSub final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 12; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _bookTs.push_back(ev.update.exchangeTsNs);
  }

  void onTrade(const TradeEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _tradeTs.push_back(ev.trade.exchangeTsNs);
  }

  std::vector<UnixNanos> bookTimestamps()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _bookTs;
  }

  std::vector<UnixNanos> tradeTimestamps()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _tradeTs;
  }

 private:
  std::mutex _m;
  std::vector<UnixNanos> _bookTs;
  std::vector<UnixNanos> _tradeTs;
};

// 2023-11-14T22:13:20Z, i.e. deep in the past relative to "now" in any CI
// run -- if exchangeTsNs came from processing time instead of the venue's
// own field, it would land within a few seconds of "now", not here.
constexpr int64_t kVenueTimestampMs = 1700000000000LL;

}  // namespace

TEST(PolymarketTimestamps, BookEventUsesVenueTimestamp)
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  CapturingSub sub;
  bookBus.subscribe(&sub);
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = "poly_ts_book.log";
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  PolymarketConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};
  PolymarketExchangeConnector connector(cfg, &bookBus, &tradeBus, &registry, logger);

  const UnixNanos beforeCall = nowUnixNanos();

  connector.handleMessage(
      R"({"event_type":"book","asset_id":"TOKEN","market":"0xabc",)"
      R"("bids":[{"price":"0.40","size":"100"}],"asks":[{"price":"0.60","size":"80"}],)"
      R"("timestamp":")" +
      std::to_string(kVenueTimestampMs) + R"("})");

  bookBus.flush();
  bookBus.stop();
  tradeBus.stop();

  const auto ts = sub.bookTimestamps();
  ASSERT_EQ(ts.size(), 1u);

  const UnixNanos expected = msToUnixNs(kVenueTimestampMs);
  EXPECT_EQ(ts[0], expected) << "exchangeTsNs must be the venue's own timestamp field, not "
                                "processing time";
  // Sanity: the venue timestamp is indeed far in the past relative to
  // "now" (beforeCall - ts[0] is a large positive number of nanoseconds)
  // -- if this ever fires, the fixture's constant needs updating, not the
  // connector.
  EXPECT_GT((beforeCall - ts[0]).count(), 0)
      << "test fixture's venue timestamp should be in the past relative to `now`";
}

TEST(PolymarketTimestamps, PriceChangeEventUsesVenueTimestamp)
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  CapturingSub sub;
  bookBus.subscribe(&sub);
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = "poly_ts_delta.log";
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  PolymarketConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};
  PolymarketExchangeConnector connector(cfg, &bookBus, &tradeBus, &registry, logger);

  connector.handleMessage(
      R"([{"event_type":"book","asset_id":"TOKEN","bids":[{"price":"0.40","size":"100"}],"asks":[{"price":"0.60","size":"80"}]}])");
  connector.handleMessage(
      R"({"event_type":"price_change","market":"0xabc","price_changes":[)"
      R"({"asset_id":"TOKEN","price":"0.41","size":"50","side":"BUY","hash":"h"}],"timestamp":)" +
      std::to_string(kVenueTimestampMs) + "}");

  bookBus.flush();
  bookBus.stop();
  tradeBus.stop();

  const auto ts = sub.bookTimestamps();
  ASSERT_EQ(ts.size(), 2u);  // initial snapshot, then the delta

  const UnixNanos expected = msToUnixNs(kVenueTimestampMs);
  EXPECT_EQ(ts[1], expected) << "the delta's exchangeTsNs must come from the message's own "
                                "\"timestamp\" field";
}

TEST(PolymarketTimestamps, TradeEventUsesVenueTimestamp)
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  CapturingSub sub;
  tradeBus.subscribe(&sub);
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = "poly_ts_trade.log";
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  PolymarketConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};
  PolymarketExchangeConnector connector(cfg, &bookBus, &tradeBus, &registry, logger);

  connector.handleMessage(
      R"({"event_type":"last_trade_price","asset_id":"TOKEN","price":"0.55","size":"10",)"
      R"("side":"BUY","timestamp":")" +
      std::to_string(kVenueTimestampMs) + R"("})");

  tradeBus.flush();
  bookBus.stop();
  tradeBus.stop();

  const auto ts = sub.tradeTimestamps();
  ASSERT_EQ(ts.size(), 1u);

  const UnixNanos expected = msToUnixNs(kVenueTimestampMs);
  EXPECT_EQ(ts[0], expected);
}
