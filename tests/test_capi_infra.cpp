#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <cmath>

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
