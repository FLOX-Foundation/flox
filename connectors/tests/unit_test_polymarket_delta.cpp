/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol test: Polymarket price_change (incremental) messages must
 * mutate the book, not be silently dropped. Raw frames are fed straight into
 * handleMessage; no sockets.
 */

#include "flox-connectors/polymarket/polymarket_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/log/atomic_logger.h>

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
  auto dir = std::filesystem::temp_directory_path() / "flox_poly_delta_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct SeenUpdate
{
  BookUpdateType type;
  std::vector<std::pair<double, double>> bids;
  std::vector<std::pair<double, double>> asks;
};

class CapturingSub final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 11; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    SeenUpdate u;
    u.type = ev.update.type;
    for (const auto& l : ev.update.bids)
    {
      u.bids.emplace_back(l.price.toDouble(), l.quantity.toDouble());
    }
    for (const auto& l : ev.update.asks)
    {
      u.asks.emplace_back(l.price.toDouble(), l.quantity.toDouble());
    }
    _updates.push_back(std::move(u));
  }

  std::vector<SeenUpdate> updates()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _updates;
  }

 private:
  std::mutex _m;
  std::vector<SeenUpdate> _updates;
};

}  // namespace

TEST(PolymarketDelta, PriceChangeAppliesAsDelta)
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
  logOpts.basename = "poly_delta.log";
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  PolymarketConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};
  PolymarketExchangeConnector connector(cfg, &bookBus, &tradeBus, &registry, logger);

  // Snapshot first (array form), then an incremental price_change.
  connector.handleMessage(
      R"([{"event_type":"book","asset_id":"TOKEN","bids":[{"price":"0.40","size":"100"}],"asks":[{"price":"0.60","size":"80"}]}])");
  connector.handleMessage(
      R"({"event_type":"price_change","market":"0xabc","price_changes":[)"
      R"({"asset_id":"TOKEN","price":"0.41","size":"50","side":"BUY","hash":"h"},)"
      R"({"asset_id":"TOKEN","price":"0.60","size":"0","side":"SELL","hash":"h"}],"timestamp":1})");

  bookBus.flush();
  bookBus.stop();
  tradeBus.stop();

  const auto ups = sub.updates();
  ASSERT_EQ(ups.size(), 2u);

  EXPECT_EQ(ups[0].type, BookUpdateType::SNAPSHOT);
  ASSERT_EQ(ups[0].bids.size(), 1u);
  EXPECT_DOUBLE_EQ(ups[0].bids[0].first, 0.40);

  // The delta carried both a new bid level and a removed ask (size 0).
  EXPECT_EQ(ups[1].type, BookUpdateType::DELTA);
  ASSERT_EQ(ups[1].bids.size(), 1u);
  EXPECT_DOUBLE_EQ(ups[1].bids[0].first, 0.41);
  EXPECT_DOUBLE_EQ(ups[1].bids[0].second, 50.0);
  ASSERT_EQ(ups[1].asks.size(), 1u);
  EXPECT_DOUBLE_EQ(ups[1].asks[0].first, 0.60);
  EXPECT_DOUBLE_EQ(ups[1].asks[0].second, 0.0);  // level removal
}
