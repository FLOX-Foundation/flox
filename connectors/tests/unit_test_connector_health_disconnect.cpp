/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline feed-health test: a transport close is the one thing every connector
 * already observes -- each of the four installs an onClose handler -- and all
 * four do nothing with it but write a log line. IExchangeConnector declares
 * DisconnectCallback and emitDisconnect for exactly this, and neither has a
 * single caller anywhere in the tree, so a supervisor cannot learn that a feed
 * went away.
 *
 * The close is delivered through a public entry point rather than a live
 * socket, mirroring how handleMessage is public so protocol behaviour stays
 * testable offline.
 *
 * needs, per connector (Bybit, Bitget, Hyperliquid, Polymarket):
 *   void handleDisconnect(int code, std::string_view reason);
 * public, invoked from the connector's own ws onClose handler, calling
 * IExchangeConnector::emitDisconnect(reason).
 */

#include "flox-connectors/bitget/bitget_exchange_connector.h"
#include "flox-connectors/bybit/bybit_exchange_connector.h"
#include "flox-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "flox-connectors/polymarket/polymarket_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

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
  auto dir = std::filesystem::temp_directory_path() / "flox_conn_disconnect_test_logs";
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = basename;
  return std::make_shared<AtomicLogger>(opts);
}

class Recorder
{
 public:
  void install(IExchangeConnector& c)
  {
    c.setErrorCallbacks(
        [this](std::string_view reason)
        {
          std::lock_guard<std::mutex> lk(_m);
          _reasons.emplace_back(reason);
        },
        [](uint64_t, uint64_t)
        {
        },
        [](SymbolId, uint64_t)
        {
        });
  }

  std::vector<std::string> reasons()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _reasons;
  }

 private:
  std::mutex _m;
  std::vector<std::string> _reasons;
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
    book.stop();
    trade.stop();
  }
};

}  // namespace

TEST(ConnectorHealthDisconnect, BybitReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

TEST(ConnectorHealthDisconnect, BitgetReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  BitgetConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};

  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

TEST(ConnectorHealthDisconnect, HyperliquidReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  HyperliquidConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.symbols = {"BTC"};

  HyperliquidExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                         makeLogger("hyperliquid_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

TEST(ConnectorHealthDisconnect, PolymarketReportsATransportClose)
{
  Buses buses;
  SymbolRegistry registry;

  PolymarketConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};

  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("polymarket_disconnect.log"));
  Recorder rec;
  rec.install(connector);

  connector.handleDisconnect(1006, "abnormal closure");

  const auto reasons = rec.reasons();
  ASSERT_EQ(reasons.size(), 1u) << "a transport close must reach emitDisconnect";
  EXPECT_NE(reasons[0].find("abnormal closure"), std::string::npos);
}

// Control: a connector with no error callbacks installed must survive the same
// close -- emitDisconnect on an empty MoveOnlyFunction is a no-op, and wiring
// the call must not turn a disconnect into a crash.
TEST(ConnectorHealthDisconnect, CloseWithNoCallbacksInstalledIsHarmless)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_disconnect_nocb.log"));

  EXPECT_NO_THROW(connector.handleDisconnect(1000, "normal closure"));
}
