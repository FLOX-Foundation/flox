#include "flox/capi/flox_capi.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <vector>

// ============================================================
// Order book
// ============================================================

TEST(CapiBookTest, CreateAndQuery)
{
  FloxBookHandle book = flox_book_create(0.01);
  ASSERT_NE(book, nullptr);

  double p = 0;
  EXPECT_EQ(flox_book_best_bid(book, &p), 0);
  EXPECT_EQ(flox_book_best_ask(book, &p), 0);

  double bids_p[] = {100.0, 99.99};
  double bids_q[] = {1.5, 2.0};
  double asks_p[] = {100.01, 100.02};
  double asks_q[] = {0.5, 1.0};
  flox_book_apply_snapshot(book, bids_p, bids_q, 2, asks_p, asks_q, 2);

  EXPECT_EQ(flox_book_best_bid(book, &p), 1);
  EXPECT_NEAR(p, 100.0, 0.001);

  EXPECT_EQ(flox_book_best_ask(book, &p), 1);
  EXPECT_NEAR(p, 100.01, 0.001);

  double mid = 0;
  EXPECT_EQ(flox_book_mid(book, &mid), 1);
  EXPECT_NEAR(mid, 100.005, 0.001);

  double spread = 0;
  EXPECT_EQ(flox_book_spread(book, &spread), 1);
  EXPECT_NEAR(spread, 0.01, 0.001);

  EXPECT_EQ(flox_book_is_crossed(book), 0);

  double prices[10], qtys[10];
  uint32_t n = flox_book_get_bids(book, prices, qtys, 10);
  EXPECT_GE(n, 1u);
  EXPECT_NEAR(prices[0], 100.0, 0.001);

  flox_book_clear(book);
  EXPECT_EQ(flox_book_best_bid(book, &p), 0);

  flox_book_destroy(book);
}

// ============================================================
// Executor
// ============================================================

TEST(CapiExecutorTest, SubmitAndFill)
{
  FloxExecutorHandle exec = flox_executor_create();
  ASSERT_NE(exec, nullptr);

  flox_executor_submit_order(exec, 1, 0, 100.0, 1.0, 0, 1);
  flox_executor_on_bar(exec, 1, 100.0);

  EXPECT_GE(flox_executor_fill_count(exec), 0u);

  flox_executor_destroy(exec);
}

// ============================================================
// Position tracker
// ============================================================

TEST(CapiPositionTest, TrackFills)
{
  FloxPositionTrackerHandle tracker = flox_position_tracker_create(0);
  ASSERT_NE(tracker, nullptr);

  flox_position_tracker_on_fill(tracker, 1, 0, 100.0, 1.0);
  EXPECT_NEAR(flox_position_tracker_position(tracker, 1), 1.0, 0.001);
  EXPECT_NEAR(flox_position_tracker_avg_entry(tracker, 1), 100.0, 0.01);

  flox_position_tracker_on_fill(tracker, 1, 1, 110.0, 1.0);
  EXPECT_NEAR(flox_position_tracker_position(tracker, 1), 0.0, 0.001);
  EXPECT_GT(flox_position_tracker_realized_pnl(tracker, 1), 0.0);

  flox_position_tracker_destroy(tracker);
}

// ============================================================
// Volume profile
// ============================================================

TEST(CapiProfileTest, VolumeProfile)
{
  FloxVolumeProfileHandle vp = flox_volume_profile_create(0.01);
  ASSERT_NE(vp, nullptr);

  flox_volume_profile_add_trade(vp, 100.0, 10.0, 1);
  flox_volume_profile_add_trade(vp, 100.0, 5.0, 0);
  flox_volume_profile_add_trade(vp, 101.0, 3.0, 1);

  EXPECT_GT(flox_volume_profile_total_volume(vp), 0.0);
  EXPECT_GT(flox_volume_profile_num_levels(vp), 0u);

  flox_volume_profile_clear(vp);
  EXPECT_EQ(flox_volume_profile_num_levels(vp), 0u);

  flox_volume_profile_destroy(vp);
}

// ============================================================
// Stats
// ============================================================

TEST(CapiStatsTest, Correlation)
{
  double x[] = {1, 2, 3, 4, 5};
  double y[] = {1, 2, 3, 4, 5};
  EXPECT_NEAR(flox_stat_correlation(x, y, 5), 1.0, 0.001);

  double z[] = {5, 4, 3, 2, 1};
  EXPECT_NEAR(flox_stat_correlation(x, z, 5), -1.0, 0.001);
}

TEST(CapiStatsTest, ProfitFactorAndWinRate)
{
  double pnl[] = {100, -50, 200, -30};
  EXPECT_GT(flox_stat_profit_factor(pnl, 4), 1.0);
  EXPECT_NEAR(flox_stat_win_rate(pnl, 4), 0.5, 0.001);
}

TEST(CapiStatsTest, BootstrapCI)
{
  double data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  double lo, med, hi;
  flox_stat_bootstrap_ci(data, 10, 0.95, 1000, &lo, &med, &hi);
  EXPECT_LT(lo, med);
  EXPECT_LT(med, hi);
  EXPECT_NEAR(med, 5.5, 1.0);
}

TEST(CapiStatsTest, PermutationTest)
{
  // Very different groups — should be significant
  double g1[] = {1, 2, 3, 4, 5};
  double g2[] = {100, 200, 300, 400, 500};
  double pval = flox_stat_permutation_test(g1, 5, g2, 5, 5000);
  EXPECT_LT(pval, 0.05);

  // Same distribution — should NOT be significant
  double g3[] = {1, 2, 3, 4, 5};
  double g4[] = {1, 2, 3, 4, 5};
  double pval2 = flox_stat_permutation_test(g3, 5, g4, 5, 5000);
  EXPECT_GT(pval2, 0.5);
}

// ============================================================
// L3 Book
// ============================================================

TEST(CapiL3BookTest, AddRemoveQuery)
{
  FloxL3BookHandle book = flox_l3_book_create();
  ASSERT_NE(book, nullptr);

  flox_l3_book_add_order(book, 1, 100.0, 1.0, 0);
  flox_l3_book_add_order(book, 2, 101.0, 0.5, 1);

  double p = 0;
  EXPECT_EQ(flox_l3_book_best_bid(book, &p), 1);
  EXPECT_NEAR(p, 100.0, 0.01);

  EXPECT_EQ(flox_l3_book_best_ask(book, &p), 1);
  EXPECT_NEAR(p, 101.0, 0.01);

  flox_l3_book_remove_order(book, 1);
  EXPECT_EQ(flox_l3_book_best_bid(book, &p), 0);

  flox_l3_book_destroy(book);
}

// ============================================================
// Order tracker
// ============================================================

TEST(CapiOrderTrackerTest, Lifecycle)
{
  FloxOrderTrackerHandle tracker = flox_order_tracker_create();
  ASSERT_NE(tracker, nullptr);

  EXPECT_EQ(flox_order_tracker_active_count(tracker), 0u);

  flox_order_tracker_on_submitted(tracker, 1, 1, 0, 100.0, 1.0);
  EXPECT_EQ(flox_order_tracker_active_count(tracker), 1u);
  EXPECT_EQ(flox_order_tracker_is_active(tracker, 1), 1);

  flox_order_tracker_on_filled(tracker, 1, 1.0);
  EXPECT_EQ(flox_order_tracker_is_active(tracker, 1), 0);
  EXPECT_EQ(flox_order_tracker_active_count(tracker), 0u);

  flox_order_tracker_destroy(tracker);
}

// ============================================================
// Replay book reads (parity with Python; same C path used by Node/Codon/QuickJS)
// ============================================================

TEST(CapiReplayTest, ReadBboAndBookUpdates)
{
  using namespace flox::replay;

  auto dir = std::filesystem::temp_directory_path() / "flox_test_capi_replay";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  constexpr int kEvents = 10;
  constexpr uint16_t kBidCount = 3;
  constexpr uint16_t kAskCount = 4;

  // Write 10 book updates via the C++ writer.
  {
    WriterConfig wcfg{.output_dir = dir};
    BinaryLogWriter writer(wcfg);

    for (int i = 0; i < kEvents; ++i)
    {
      BookRecordHeader header{};
      header.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      header.recv_ts_ns = header.exchange_ts_ns + 100;
      header.seq = 1000 + i;
      header.symbol_id = 7;
      header.bid_count = kBidCount;
      header.ask_count = kAskCount;
      header.type = (i == 0) ? 0 : 1;  // 0=snapshot, 1=delta

      std::vector<BookLevel> bids(kBidCount);
      std::vector<BookLevel> asks(kAskCount);
      for (uint16_t j = 0; j < kBidCount; ++j)
      {
        bids[j] = {(50000LL - j) * 1000000LL, (j + 1LL) * 1000000LL};
      }
      for (uint16_t j = 0; j < kAskCount; ++j)
      {
        asks[j] = {(50001LL + j) * 1000000LL, (j + 1LL) * 1000000LL};
      }

      EXPECT_TRUE(writer.writeBook(header, bids, asks));
    }
    writer.close();
  }

  // BBO read.
  FloxDataReaderHandle r = flox_data_reader_create(dir.string().c_str());
  ASSERT_NE(r, nullptr);

  uint64_t bbo_count = flox_data_reader_read_bbo(r, nullptr, 0);
  EXPECT_EQ(bbo_count, static_cast<uint64_t>(kEvents));

  std::vector<FloxBBO> bbos(bbo_count);
  uint64_t got = flox_data_reader_read_bbo(r, bbos.data(), bbo_count);
  EXPECT_EQ(got, bbo_count);
  for (uint64_t i = 0; i < got; ++i)
  {
    EXPECT_EQ(bbos[i].symbol_id, 7u);
    EXPECT_EQ(bbos[i].seq, static_cast<int64_t>(1000 + i));
    EXPECT_EQ(bbos[i].bid_price_raw, 50000LL * 1000000LL);
    EXPECT_EQ(bbos[i].ask_price_raw, 50001LL * 1000000LL);
  }

  // Full book updates (headers + flat levels).
  uint64_t total_levels = 0;
  uint64_t event_count = flox_data_reader_count_book_updates(r, &total_levels);
  EXPECT_EQ(event_count, static_cast<uint64_t>(kEvents));
  EXPECT_EQ(total_levels, static_cast<uint64_t>(kEvents) * (kBidCount + kAskCount));

  std::vector<FloxBookUpdateHeader> headers(event_count);
  std::vector<FloxLevel> levels(total_levels);
  uint64_t events_got = flox_data_reader_read_book_updates(
      r, headers.data(), event_count, levels.data(), total_levels);
  EXPECT_EQ(events_got, event_count);

  for (uint64_t i = 0; i < events_got; ++i)
  {
    const auto& h = headers[i];
    EXPECT_EQ(h.symbol_id, 7u);
    EXPECT_EQ(h.bid_count, kBidCount);
    EXPECT_EQ(h.ask_count, kAskCount);
    EXPECT_EQ(h.level_offset, i * (kBidCount + kAskCount));

    for (uint16_t k = 0; k < kBidCount; ++k)
    {
      EXPECT_EQ(levels[h.level_offset + k].side, 0);
    }
    for (uint16_t k = 0; k < kAskCount; ++k)
    {
      EXPECT_EQ(levels[h.level_offset + kBidCount + k].side, 1);
    }
  }

  flox_data_reader_destroy(r);
  std::filesystem::remove_all(dir);
}

// ============================================================
// IndicatorGraph (batch)
// ============================================================

namespace
{
// Test fixture: a node fn that returns the close array * 2.
static const double* doubleClose(void* user_data, FloxIndicatorGraphHandle g, uint32_t sym,
                                 size_t* out_len)
{
  auto* state = static_cast<std::vector<double>*>(user_data);
  size_t n = 0;
  const double* c = flox_indicator_graph_close(g, sym, &n);
  state->resize(n);
  for (size_t i = 0; i < n; ++i)
  {
    (*state)[i] = c[i] * 2.0;
  }
  *out_len = n;
  return state->data();
}

// dependent: returns close + parent
static const double* sumWithDouble(void* user_data, FloxIndicatorGraphHandle g, uint32_t sym,
                                   size_t* out_len)
{
  auto* state = static_cast<std::vector<double>*>(user_data);
  size_t n = 0;
  const double* c = flox_indicator_graph_close(g, sym, &n);
  size_t pn = 0;
  const double* p = flox_indicator_graph_get(g, sym, "double_close", &pn);
  if (!p || pn != n)
  {
    *out_len = 0;
    return nullptr;
  }
  state->resize(n);
  for (size_t i = 0; i < n; ++i)
  {
    (*state)[i] = c[i] + p[i];
  }
  *out_len = n;
  return state->data();
}
}  // namespace

TEST(CapiGraphTest, BasicComputeAndDeps)
{
  auto g = flox_indicator_graph_create();
  ASSERT_NE(g, nullptr);

  std::vector<double> close = {1.0, 2.0, 3.0, 4.0, 5.0};
  flox_indicator_graph_set_bars(g, 0, close.data(), nullptr, nullptr, nullptr, close.size());

  std::vector<double> stateA, stateB;
  flox_indicator_graph_add_node(g, "double_close", nullptr, 0, doubleClose, &stateA);
  const char* deps[] = {"double_close"};
  flox_indicator_graph_add_node(g, "sum", deps, 1, sumWithDouble, &stateB);

  size_t len = 0;
  const double* out = flox_indicator_graph_require(g, 0, "sum", &len);
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(len, 5u);
  for (size_t i = 0; i < 5; ++i)
  {
    EXPECT_NEAR(out[i], close[i] * 3.0, 1e-12);
  }

  // get on cached node returns the same pointer.
  size_t cl = 0;
  const double* cached = flox_indicator_graph_get(g, 0, "double_close", &cl);
  ASSERT_NE(cached, nullptr);
  EXPECT_EQ(cl, 5u);

  // Unknown node -> nullptr.
  size_t l2 = 999;
  const double* missing = flox_indicator_graph_require(g, 0, "nope", &l2);
  EXPECT_EQ(missing, nullptr);
  EXPECT_EQ(l2, 0u);

  flox_indicator_graph_destroy(g);
}

// ============================================================
// StreamingIndicatorGraph (C API)
// ============================================================

TEST(CapiStreamingGraphTest, StepAndCurrent)
{
  auto sg = flox_streaming_graph_create();
  ASSERT_NE(sg, nullptr);

  std::vector<double> stateA, stateB;
  flox_streaming_graph_add_node(sg, "double_close", nullptr, 0, doubleClose, &stateA);
  const char* deps[] = {"double_close"};
  flox_streaming_graph_add_node(sg, "sum", deps, 1, sumWithDouble, &stateB);

  // Before any steps current returns NaN.
  EXPECT_TRUE(std::isnan(flox_streaming_graph_current(sg, 0, "double_close")));
  EXPECT_EQ(flox_streaming_graph_bar_count(sg, 0), 0u);

  std::vector<double> closes = {1.0, 2.0, 3.0, 4.0, 5.0};
  for (size_t i = 0; i < closes.size(); ++i)
  {
    double c = closes[i];
    flox_streaming_graph_step(sg, 0, c, c, c, c, 0.0);
    EXPECT_EQ(flox_streaming_graph_bar_count(sg, 0), i + 1);

    // double_close node: returns close * 2 for the last bar
    EXPECT_NEAR(flox_streaming_graph_current(sg, 0, "double_close"), c * 2.0, 1e-12);
    // sum node: returns close + double_close = close * 3
    EXPECT_NEAR(flox_streaming_graph_current(sg, 0, "sum"), c * 3.0, 1e-12);
  }

  flox_streaming_graph_destroy(sg);
}

TEST(CapiStreamingGraphTest, ParityWithBatch)
{
  // Verify streaming current values after N steps == batch result[N-1].
  std::vector<double> stateA, stateB;

  auto sg = flox_streaming_graph_create();
  flox_streaming_graph_add_node(sg, "double_close", nullptr, 0, doubleClose, &stateA);
  const char* deps[] = {"double_close"};
  flox_streaming_graph_add_node(sg, "sum", deps, 1, sumWithDouble, &stateB);

  std::vector<double> closes = {10.0, 20.0, 30.0, 40.0, 50.0};
  for (double c : closes)
  {
    flox_streaming_graph_step(sg, 0, c, c, c, c, 0.0);
  }

  // Batch run on the same data.
  std::vector<double> batchStateA, batchStateB;
  auto bg = flox_indicator_graph_create();
  flox_indicator_graph_add_node(bg, "double_close", nullptr, 0, doubleClose, &batchStateA);
  flox_indicator_graph_add_node(bg, "sum", deps, 1, sumWithDouble, &batchStateB);
  flox_indicator_graph_set_bars(bg, 0, closes.data(), nullptr, nullptr, nullptr, closes.size());

  size_t len = 0;
  const double* batchSum = flox_indicator_graph_require(bg, 0, "sum", &len);
  ASSERT_EQ(len, closes.size());

  // Streaming current == batch last element.
  EXPECT_NEAR(flox_streaming_graph_current(sg, 0, "sum"), batchSum[len - 1], 1e-12);

  flox_indicator_graph_destroy(bg);
  flox_streaming_graph_destroy(sg);
}

TEST(CapiStreamingGraphTest, Reset)
{
  std::vector<double> stateA;
  auto sg = flox_streaming_graph_create();
  flox_streaming_graph_add_node(sg, "double_close", nullptr, 0, doubleClose, &stateA);

  flox_streaming_graph_step(sg, 0, 5.0, 5.0, 5.0, 5.0, 0.0);
  EXPECT_NEAR(flox_streaming_graph_current(sg, 0, "double_close"), 10.0, 1e-12);
  EXPECT_EQ(flox_streaming_graph_bar_count(sg, 0), 1u);

  flox_streaming_graph_reset(sg, 0);
  EXPECT_EQ(flox_streaming_graph_bar_count(sg, 0), 0u);
  EXPECT_TRUE(std::isnan(flox_streaming_graph_current(sg, 0, "double_close")));

  // Can resume stepping after reset.
  flox_streaming_graph_step(sg, 0, 7.0, 7.0, 7.0, 7.0, 0.0);
  EXPECT_NEAR(flox_streaming_graph_current(sg, 0, "double_close"), 14.0, 1e-12);

  flox_streaming_graph_destroy(sg);
}
