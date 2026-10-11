/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline tests for the Hyperliquid bbo channel. The venue throttles l2Book
 * snapshots server-side to ~0.2/s per coin; bbo pushes on every top-of-book
 * change. Recorded frames go into the connector's public handleMessage seam,
 * no socket involved — the same technique unit_test_hyperliquid_fill_contract
 * uses.
 *
 * What is pinned:
 *
 *  - A bbo frame becomes a one-level SNAPSHOT that carries recvNs and
 *    sourceExchange, so CompositeBookMatrix accepts it and its staleness
 *    sweep sees the venue ticking at bbo rate, not at l2Book rate.
 *
 *  - Between two l2Book snapshots a bbo frame moves the cross-venue top of
 *    book. That is the point of the channel: without it the best bid/ask is
 *    up to ~5 seconds old.
 *
 *  - A null side in the bbo array (an empty book side) stays empty instead
 *    of becoming a parse error or a phantom level.
 *
 *  - The subscription is opt-in: the default config does not enable it, so
 *    existing deployments keep their event mix.
 */

#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/book/composite_book_matrix.h>
#include <lrvx/book/events/book_update_event.h>
#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_hl_bbo_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = name;
  return std::make_shared<AtomicLogger>(logOpts);
}

struct SeenBook
{
  uint64_t recvNs;
  ExchangeId sourceExchange;
  BookUpdateType type;
  int64_t exchangeTsNs;
  std::vector<std::pair<int64_t, int64_t>> bids;  // (price raw, qty raw)
  std::vector<std::pair<int64_t, int64_t>> asks;
};

class BookCapture final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 71; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    SeenBook seen;
    seen.recvNs = ev.recvNs.raw();
    seen.sourceExchange = ev.sourceExchange;
    seen.type = ev.update.type;
    seen.exchangeTsNs = ev.update.exchangeTsNs.raw();
    for (const auto& lvl : ev.update.bids)
    {
      seen.bids.emplace_back(lvl.price.raw(), lvl.quantity.raw());
    }
    for (const auto& lvl : ev.update.asks)
    {
      seen.asks.emplace_back(lvl.price.raw(), lvl.quantity.raw());
    }
    std::lock_guard<std::mutex> lk(_m);
    _books.push_back(std::move(seen));
  }

  std::vector<SeenBook> books()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _books;
  }

 private:
  std::mutex _m;
  std::vector<SeenBook> _books;
};

struct FeedFixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<HyperliquidExchangeConnector> connector;
  BookCapture bookCapture;
  CompositeBookMatrix<4> matrix;

  explicit FeedFixture(const char* logName)
  {
    logger = makeLogger(logName);
    registry.registerExchange("hyperliquid");

    HyperliquidConfig cfg;
    cfg.wsEndpoint = "wss://unused.invalid";
    cfg.symbols = {"BTC"};
    connector =
        std::make_unique<HyperliquidExchangeConnector>(cfg, &bookBus, &tradeBus, &registry, logger);

    matrix.setId(72);
    bookBus.subscribe(&bookCapture);
    bookBus.subscribe(&matrix);
    bookBus.start();
    tradeBus.start();
  }

  ~FeedFixture()
  {
    bookBus.stop();
    tradeBus.stop();
  }
};

constexpr const char* kL2BookFrame =
    R"({"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,"levels":)"
    R"([[{"px":"60000","sz":"3","n":1}],[{"px":"60010","sz":"2","n":1}]]}})";

constexpr const char* kBboFrame =
    R"({"channel":"bbo","data":{"coin":"BTC","time":1700000001000,"bbo":)"
    R"([{"px":"60005","sz":"1.5","n":3},{"px":"60009","sz":"2","n":1}]}})";

constexpr const char* kBboAskOnlyFrame =
    R"({"channel":"bbo","data":{"coin":"BTC","time":1700000002000,"bbo":)"
    R"([null,{"px":"60009","sz":"2","n":1}]}})";

}  // namespace

TEST(HyperliquidBbo, SubscriptionIsOptIn)
{
  // Existing deployments keep their event mix: a one-level snapshot between
  // l2Book snapshots collapses held depth, so nobody gets that silently.
  EXPECT_FALSE(HyperliquidConfig{}.subscribeBbo);
}

TEST(HyperliquidBbo, BboFrameIsAOneLevelSnapshotWithStamps)
{
  FeedFixture f("hl_bbo_snapshot.log");
  const ExchangeId expected = f.registry.getExchangeId(f.connector->exchangeId());
  ASSERT_NE(expected, InvalidExchangeId);

  f.connector->handleMessage(kBboFrame);
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  const auto& ev = books[0];

  EXPECT_EQ(ev.type, BookUpdateType::SNAPSHOT);
  ASSERT_EQ(ev.bids.size(), 1u);
  ASSERT_EQ(ev.asks.size(), 1u);
  EXPECT_EQ(ev.bids[0].first, Price::fromDouble(60005.0).raw());
  EXPECT_EQ(ev.bids[0].second, Quantity::fromDouble(1.5).raw());
  EXPECT_EQ(ev.asks[0].first, Price::fromDouble(60009.0).raw());
  EXPECT_EQ(ev.exchangeTsNs, 1700000001000LL * 1'000'000LL);
  EXPECT_NE(ev.recvNs, 0u) << "recvNs gates CompositeBookMatrix::checkStaleness; at zero the "
                              "venue is skipped and a frozen feed stays quotable";
  EXPECT_EQ(ev.sourceExchange, expected)
      << "sourceExchange must be the connector's exchange id; InvalidExchangeId makes "
         "CompositeBookMatrix drop the update";
}

TEST(HyperliquidBbo, BboMovesTheTopBetweenL2Snapshots)
{
  FeedFixture f("hl_bbo_refresh.log");

  f.connector->handleMessage(kL2BookFrame);
  f.connector->handleMessage(kBboFrame);
  f.bookBus.flush();

  const auto sym = f.registry.getSymbolId("hyperliquid", "BTC");
  ASSERT_TRUE(sym.has_value());

  // The l2Book snapshot put the top at 60000/60010; the bbo frame that
  // followed is the venue's newer word on it.
  auto bid = f.matrix.bestBid(*sym);
  ASSERT_TRUE(bid.valid);
  EXPECT_EQ(bid.priceRaw, Price::fromDouble(60005.0).raw());

  auto ask = f.matrix.bestAsk(*sym);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(ask.priceRaw, Price::fromDouble(60009.0).raw());
}

TEST(HyperliquidBbo, NullSideStaysEmpty)
{
  FeedFixture f("hl_bbo_null_side.log");

  f.connector->handleMessage(kBboAskOnlyFrame);
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_TRUE(books[0].bids.empty());
  ASSERT_EQ(books[0].asks.size(), 1u);
  EXPECT_EQ(books[0].asks[0].first, Price::fromDouble(60009.0).raw());
}
