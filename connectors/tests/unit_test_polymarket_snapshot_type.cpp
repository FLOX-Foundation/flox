/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol test: a Polymarket "book" frame is a full snapshot and must
 * be published as one. processBookSnapshot never assigns update.type, and
 * BookUpdateEvent::clear() resets only bids/asks, so a snapshot that acquires a
 * pool slot last used by a price_change inherits DELTA -- the consumer merges
 * the snapshot into a book it should have replaced, and stale levels survive
 * forever. The existing delta test passes only because its snapshot is the
 * first event on a fresh pool.
 *
 * No sockets: raw frames go straight into handleMessage.
 */

#include "flox-connectors/polymarket/polymarket_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/engine/engine_config.h>
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

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_poly_snapshot_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

class TypeCapturingSub final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 31; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _types.push_back(ev.update.type);
    _bidCounts.push_back(ev.update.bids.size());
  }

  std::vector<BookUpdateType> types()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _types;
  }

  std::vector<size_t> bidCounts()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _bidCounts;
  }

 private:
  std::mutex _m;
  std::vector<BookUpdateType> _types;
  std::vector<size_t> _bidCounts;
};

std::string snapshotFrame()
{
  return R"([{"event_type":"book","asset_id":"TOKEN",)"
         R"("bids":[{"price":"0.40","size":"100"}],)"
         R"("asks":[{"price":"0.60","size":"80"}]}])";
}

std::string priceChangeFrame(const char* px)
{
  std::string s = R"({"event_type":"price_change","market":"0xabc","price_changes":[)"
                  R"({"asset_id":"TOKEN","price":")";
  s += px;
  s += R"(","size":"50","side":"BUY","hash":"h"}],"timestamp":1})";
  return s;
}

struct Harness
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  TypeCapturingSub sub;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<PolymarketExchangeConnector> connector;

  explicit Harness(const char* logName)
  {
    bookBus.subscribe(&sub);
    bookBus.start();
    tradeBus.start();

    AtomicLoggerOptions logOpts;
    logOpts.directory = tempLogDir();
    logOpts.basename = logName;
    logger = std::make_shared<AtomicLogger>(logOpts);

    PolymarketConfig cfg;
    cfg.wsEndpoint = "wss://unused.invalid";
    cfg.tokenIds = {"TOKEN"};
    connector =
        std::make_unique<PolymarketExchangeConnector>(cfg, &bookBus, &tradeBus, &registry, logger);
  }

  ~Harness()
  {
    bookBus.flush();
    bookBus.stop();
    tradeBus.stop();
  }
};

}  // namespace

// Control (green today): the very first frame on a fresh pool is a snapshot and
// is typed as one. This is the case the existing delta test happens to cover;
// it must stay true.
TEST(PolymarketSnapshotType, FirstSnapshotOnAFreshPoolIsTypedSnapshot)
{
  Harness h("poly_snapshot_fresh.log");

  h.connector->handleMessage(snapshotFrame());
  h.bookBus.flush();

  const auto types = h.sub.types();
  ASSERT_EQ(types.size(), 1u);
  EXPECT_EQ(types[0], BookUpdateType::SNAPSHOT);
}

// The finding: once the event pool starts recycling slots, a snapshot inherits
// whatever type the slot carried last. The bus retains each Handle until its
// ring slot is overwritten, so publishing one bus-capacity worth of deltas is
// what puts a used DELTA slot back on the pool freelist; the next acquire --
// the snapshot below -- gets it.
TEST(PolymarketSnapshotType, SnapshotOnARecycledSlotIsStillTypedSnapshot)
{
  Harness h("poly_snapshot_recycled.log");

  h.connector->handleMessage(snapshotFrame());

  constexpr size_t kDeltas = config::DEFAULT_EVENTBUS_CAPACITY + 8;
  for (size_t i = 0; i < kDeltas; ++i)
  {
    h.connector->handleMessage(priceChangeFrame("0.41"));
  }
  h.bookBus.flush();

  const auto before = h.sub.types().size();
  ASSERT_GT(before, config::DEFAULT_EVENTBUS_CAPACITY)
      << "the bus ring must have wrapped for a pool slot to be recycled";

  h.connector->handleMessage(snapshotFrame());
  h.bookBus.flush();

  const auto types = h.sub.types();
  ASSERT_EQ(types.size(), before + 1u);
  EXPECT_EQ(types.back(), BookUpdateType::SNAPSHOT)
      << "a book frame is a full snapshot and must never be published as DELTA";

  // And it must be a real snapshot, not an empty event that merely carries the
  // right label: the levels from the frame are there.
  EXPECT_EQ(h.sub.bidCounts().back(), 1u);
}
