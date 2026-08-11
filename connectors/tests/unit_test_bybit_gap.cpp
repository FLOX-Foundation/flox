/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol test: Bybit orderbook update-id continuity. A delta whose
 * "u" is not lastU+1 means a dropped frame; the connector must DROP it (never
 * apply onto a stale book), count the gap, and suppress further deltas until a
 * fresh snapshot re-baselines. No sockets involved: raw frames are fed straight
 * into handleMessage.
 */

#include "flox-connectors/bybit/bybit_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace flox;

namespace
{

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_bybit_gap_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

class CapturingSub final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 7; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _events.push_back({ev.update.type, ev.seq});
  }

  struct Seen
  {
    BookUpdateType type;
    int64_t updateId;
  };

  std::vector<Seen> events()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _events;
  }

 private:
  std::mutex _m;
  std::vector<Seen> _events;
};

std::string bookFrame(const char* type, int64_t u, int64_t seq)
{
  std::string s = R"({"topic":"orderbook.50.BTCUSDT","type":")";
  s += type;
  s +=
      R"(","ts":1700000000000,"cts":1700000000000,"data":{"s":"BTCUSDT","b":[["100","1"]],"a":[["101","2"]],"u":)";
  s += std::to_string(u);
  s += R"(,"seq":)";
  s += std::to_string(seq);
  s += "}}";
  return s;
}

}  // namespace

TEST(BybitBookGap, DeltaGapDropsAndResyncs)
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  CapturingSub sub;
  bookBus.subscribe(&sub);
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;
  SymbolInfo btc{};
  btc.symbol = "BTCUSDT";
  btc.exchange = "bybit";
  btc.type = InstrumentType::Future;
  registry.registerSymbol(btc);

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};

  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = "bybit_gap_test.log";
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  BybitExchangeConnector connector(cfg, &bookBus, &tradeBus, nullptr, &registry, logger);
  // Never started: frames are fed directly; resubscribe is a no-op without a
  // socket, which is exactly what the offline path needs.

  connector.handleMessage(bookFrame("snapshot", 100, 1000));  // baseline
  connector.handleMessage(bookFrame("delta", 101, 1001));     // contiguous -> applied
  connector.handleMessage(bookFrame("delta", 105, 1002));     // GAP -> dropped, resync
  connector.handleMessage(bookFrame("delta", 106, 1003));     // in-flight -> dropped
  connector.handleMessage(bookFrame("snapshot", 200, 1010));  // fresh baseline
  connector.handleMessage(bookFrame("delta", 201, 1011));     // contiguous -> applied

  bookBus.flush();
  bookBus.stop();
  tradeBus.stop();

  EXPECT_EQ(connector.bookGapCount(), 1u);  // one gap; in-flight drops don't recount

  const auto seen = sub.events();
  ASSERT_EQ(seen.size(), 4u);  // the two gap-window deltas never reached the bus
  EXPECT_EQ(seen[0].type, BookUpdateType::SNAPSHOT);
  EXPECT_EQ(seen[0].updateId, 100);
  EXPECT_EQ(seen[1].type, BookUpdateType::DELTA);
  EXPECT_EQ(seen[1].updateId, 101);
  EXPECT_EQ(seen[2].type, BookUpdateType::SNAPSHOT);
  EXPECT_EQ(seen[2].updateId, 200);
  EXPECT_EQ(seen[3].type, BookUpdateType::DELTA);
  EXPECT_EQ(seen[3].updateId, 201);
}

TEST(BybitBookGap, DeltaBeforeSnapshotIsDropped)
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  CapturingSub sub;
  bookBus.subscribe(&sub);
  bookBus.start();
  tradeBus.start();

  SymbolRegistry registry;
  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};

  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = "bybit_gap_test2.log";
  auto logger = std::make_shared<AtomicLogger>(logOpts);

  BybitExchangeConnector connector(cfg, &bookBus, &tradeBus, nullptr, &registry, logger);

  connector.handleMessage(bookFrame("delta", 50, 900));  // no baseline -> gap path
  bookBus.flush();
  bookBus.stop();
  tradeBus.stop();

  EXPECT_EQ(connector.bookGapCount(), 1u);
  EXPECT_TRUE(sub.events().empty());  // nothing published from a baseline-less delta
}
