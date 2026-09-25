/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline feed-health test: a Bybit orderbook update-id gap is a feed-health
 * event, not just a log line. IExchangeConnector::setErrorCallbacks installs
 * the framework's only generic health surface; the connector detects the gap
 * already (it drops the delta and re-subscribes) but never reaches
 * emitSequenceGap, so a supervisor wired to those callbacks sees a book that
 * silently stops updating and no signal at all.
 *
 * No sockets: raw frames go straight into handleMessage.
 */

#include "flox-connectors/bybit/bybit_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/events/book_update_event.h>
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

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_bybit_health_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

class CapturingSub final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 21; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _seq.push_back(ev.seq);
  }

  std::vector<int64_t> sequences()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _seq;
  }

 private:
  std::mutex _m;
  std::vector<int64_t> _seq;
};

struct GapRecord
{
  uint64_t expected;
  uint64_t received;
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

struct Harness
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  CapturingSub sub;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<BybitExchangeConnector> connector;

  std::vector<GapRecord> gaps;
  std::vector<std::string> disconnects;
  std::mutex m;

  explicit Harness(const char* logName)
  {
    bookBus.subscribe(&sub);
    bookBus.start();
    tradeBus.start();

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
    logOpts.basename = logName;
    logger = std::make_shared<AtomicLogger>(logOpts);

    connector = std::make_unique<BybitExchangeConnector>(cfg, &bookBus, &tradeBus, nullptr,
                                                         &registry, logger);

    connector->setErrorCallbacks(
        [this](std::string_view reason)
        {
          std::lock_guard<std::mutex> lk(m);
          disconnects.emplace_back(reason);
        },
        [this](uint64_t expected, uint64_t received)
        {
          std::lock_guard<std::mutex> lk(m);
          gaps.push_back({expected, received});
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

  std::vector<GapRecord> seenGaps()
  {
    std::lock_guard<std::mutex> lk(m);
    return gaps;
  }
};

}  // namespace

// The finding: the gap is detected and logged, and nothing else. A supervisor
// that registered a SequenceGapCallback must be told which ids were skipped.
TEST(BybitFeedHealth, BookGapEmitsTheSequenceGapEvent)
{
  Harness h("bybit_health_gap.log");

  h.connector->handleMessage(bookFrame("snapshot", 100, 1000));
  h.connector->handleMessage(bookFrame("delta", 101, 1001));
  h.connector->handleMessage(bookFrame("delta", 105, 1002));  // gap: 102 expected

  const auto gaps = h.seenGaps();
  ASSERT_EQ(gaps.size(), 1u) << "a detected book gap must reach emitSequenceGap";
  EXPECT_EQ(gaps[0].expected, 102u);
  EXPECT_EQ(gaps[0].received, 105u);
}

// A delta arriving with no baseline is the same class of hole: the connector
// has no book to apply it to, so the consumer must hear about it.
TEST(BybitFeedHealth, DeltaWithNoBaselineEmitsTheSequenceGapEvent)
{
  Harness h("bybit_health_nobaseline.log");

  h.connector->handleMessage(bookFrame("delta", 50, 900));

  EXPECT_EQ(h.seenGaps().size(), 1u)
      << "a baseline-less delta is a gap and must reach emitSequenceGap";
}

// Control (green today): whatever the health surface ends up doing, the gap
// must keep invalidating the book -- the delta is dropped, deltas racing the
// re-subscribe are dropped too, and only a fresh snapshot re-baselines. A fix
// that emits the event but stops dropping the delta fails here.
TEST(BybitFeedHealth, GapStillDropsTheDeltaAndWaitsForAFreshSnapshot)
{
  Harness h("bybit_health_control.log");

  h.connector->handleMessage(bookFrame("snapshot", 100, 1000));
  h.connector->handleMessage(bookFrame("delta", 101, 1001));
  h.connector->handleMessage(bookFrame("delta", 105, 1002));  // gap
  h.connector->handleMessage(bookFrame("delta", 106, 1003));  // in flight
  h.connector->handleMessage(bookFrame("snapshot", 200, 1010));
  h.connector->handleMessage(bookFrame("delta", 201, 1011));

  h.bookBus.flush();

  EXPECT_EQ(h.connector->bookGapCount(), 1u);
  const auto seen = h.sub.sequences();
  ASSERT_EQ(seen.size(), 4u);
  EXPECT_EQ(seen[0], 100);
  EXPECT_EQ(seen[1], 101);
  EXPECT_EQ(seen[2], 200);
  EXPECT_EQ(seen[3], 201);
}
