/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline feed-health test: a socket that stays open while the data stops is
 * the failure mode a disconnect callback cannot catch, and it is the one that
 * keeps a dead book quoted. IExchangeConnector declares StaleDataCallback and
 * emitStaleData for it; nothing calls either, and the core's own staleness
 * check (composite_book_matrix) skips any exchange that never stamped a last
 * update, which Bitget and Hyperliquid never do.
 *
 * Time is injected rather than slept on, so the test is deterministic.
 *
 * needs, per connector (Bybit, Bitget, Hyperliquid, Polymarket):
 *   int <Config>::staleDataTimeoutMs{...};      // 0 disables the check
 *   void pollFeedHealth(MonoNanos now);         // public
 * pollFeedHealth emits emitStaleData(symbol, lastUpdateMs) for every
 * subscribed symbol whose last received update is older than the window, and
 * emits nothing for a symbol whose data is inside it. The connector must
 * record a per-symbol last-update stamp to answer that.
 *
 * needs, additionally:
 *   public void BitgetExchangeConnector::handleMessage(std::string_view);
 *   public void HyperliquidExchangeConnector::handleMessage(std::string_view);
 * -- both are private today, so a frame cannot be fed without a live socket.
 */

#include "flox-connectors/bitget/bitget_exchange_connector.h"
#include "flox-connectors/bybit/bybit_exchange_connector.h"
#include "flox-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "flox-connectors/polymarket/polymarket_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/log/atomic_logger.h>
#include <flox/util/base/time.h>

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

constexpr int kWindowMs = 1000;
constexpr uint64_t kNsPerMsU = 1'000'000ULL;

std::shared_ptr<AtomicLogger> makeLogger(const char* basename)
{
  auto dir = std::filesystem::temp_directory_path() / "flox_conn_stale_test_logs";
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = basename;
  return std::make_shared<AtomicLogger>(opts);
}

MonoNanos plusMs(MonoNanos base, uint64_t ms)
{
  return MonoNanos::fromRaw(base.raw() + ms * kNsPerMsU);
}

class StaleRecorder
{
 public:
  void install(IExchangeConnector& c)
  {
    c.setErrorCallbacks(
        [](std::string_view)
        {
        },
        [](uint64_t, uint64_t)
        {
        },
        [this](SymbolId sym, uint64_t lastUpdateMs)
        {
          std::lock_guard<std::mutex> lk(_m);
          _hits.push_back({sym, lastUpdateMs});
        });
  }

  struct Hit
  {
    SymbolId symbol;
    uint64_t lastUpdateMs;
  };

  std::vector<Hit> hits()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _hits;
  }

  size_t count()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _hits.size();
  }

 private:
  std::mutex _m;
  std::vector<Hit> _hits;
};

struct Buses
{
  BookUpdateBus book;
  TradeBus trade;

  Buses()
  {
    book.start();
    trade.start();
  }

  ~Buses()
  {
    book.flush();
    book.stop();
    trade.stop();
  }
};

std::string bybitBookFrame(int64_t u)
{
  std::string s = R"({"topic":"orderbook.50.BTCUSDT","type":"snapshot","ts":1700000000000,)"
                  R"("cts":1700000000000,"data":{"s":"BTCUSDT","b":[["100","1"]],)"
                  R"("a":[["101","2"]],"u":)";
  s += std::to_string(u);
  s += R"(,"seq":1}})";
  return s;
}

std::string bitgetBookFrame()
{
  return R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","channel":"books15",)"
         R"("instId":"BTCUSDT"},"data":[{"asks":[["101","2"]],"bids":[["100","1"]],)"
         R"("ts":"1700000000000"}],"ts":1700000000000})";
}

std::string hyperliquidBookFrame()
{
  return R"({"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,)"
         R"("levels":[[{"px":"100","sz":"1","n":1}],[{"px":"101","sz":"2","n":1}]]}})";
}

std::string polymarketBookFrame()
{
  return R"([{"event_type":"book","asset_id":"TOKEN",)"
         R"("bids":[{"price":"0.40","size":"100"}],)"
         R"("asks":[{"price":"0.60","size":"80"}]}])";
}

}  // namespace

TEST(ConnectorHealthStale, BybitReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(bybitBookFrame(100));
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u) << "a feed inside its window is not stale";

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("BTCUSDT"));

  // Fresh data clears it: the next poll inside the window is silent again.
  connector.handleMessage(bybitBookFrame(101));
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u) << "a recovered feed must not keep reporting stale";
}

TEST(ConnectorHealthStale, BitgetReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  BitgetConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(bitgetBookFrame());
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u);

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("BTCUSDT"));

  connector.handleMessage(bitgetBookFrame());
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u);
}

TEST(ConnectorHealthStale, HyperliquidReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  HyperliquidConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.symbols = {"BTC"};
  cfg.staleDataTimeoutMs = kWindowMs;

  HyperliquidExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                         makeLogger("hyperliquid_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(hyperliquidBookFrame());
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u);

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("BTC"));

  connector.handleMessage(hyperliquidBookFrame());
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u);
}

TEST(ConnectorHealthStale, PolymarketReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  PolymarketConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};
  cfg.staleDataTimeoutMs = kWindowMs;

  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("polymarket_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(polymarketBookFrame());
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u);

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("TOKEN"));

  connector.handleMessage(polymarketBookFrame());
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u);
}

// Control: a zero window disables the check, so a connector that never
// received a frame at all does not spray stale events on every poll.
TEST(ConnectorHealthStale, ZeroWindowDisablesTheCheck)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = 0;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_off.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.pollFeedHealth(plusMs(nowMonoNanos(), 60'000));
  EXPECT_EQ(rec.count(), 0u);
}
