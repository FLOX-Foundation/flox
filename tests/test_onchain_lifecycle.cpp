/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/events/order_event.h"

#include <gtest/gtest.h>
#include <string>

using namespace flox;

namespace
{

struct RecordingListener : IOrderExecutionListener
{
  RecordingListener() : IOrderExecutionListener(1) {}

  bool pendingOnchain = false;
  bool reverted = false;
  bool gasReplaced = false;
  bool filled = false;
  std::string lastTxHash;
  std::string lastRevertReason;

  void onOrderPendingOnchain(const Order&, const std::string& txHash) override
  {
    pendingOnchain = true;
    lastTxHash = txHash;
  }
  void onOrderReverted(const Order&, const std::string& reason) override
  {
    reverted = true;
    lastRevertReason = reason;
  }
  void onOrderGasReplaced(const Order&, const Order&) override { gasReplaced = true; }
  void onOrderFilled(const Order&) override { filled = true; }
};

TEST(OnchainLifecycleTest, PendingOnchainDispatchesWithTxHash)
{
  RecordingListener l;
  OrderEvent ev;
  ev.status = OrderEventStatus::PENDING_ONCHAIN;
  ev.txHash = "0xdeadbeef";
  ev.confirmations = 0;
  ev.dispatchTo(l);
  EXPECT_TRUE(l.pendingOnchain);
  EXPECT_EQ(l.lastTxHash, "0xdeadbeef");
}

TEST(OnchainLifecycleTest, RevertedCarriesReason)
{
  RecordingListener l;
  OrderEvent ev;
  ev.status = OrderEventStatus::REVERTED;
  ev.rejectReason = "insufficient liquidity";
  ev.dispatchTo(l);
  EXPECT_TRUE(l.reverted);
  EXPECT_EQ(l.lastRevertReason, "insufficient liquidity");
}

TEST(OnchainLifecycleTest, GasReplacedDispatches)
{
  RecordingListener l;
  OrderEvent ev;
  ev.status = OrderEventStatus::REPLACED_GAS;
  ev.dispatchTo(l);
  EXPECT_TRUE(l.gasReplaced);
}

// Existing CEX statuses are unaffected, and the new fields default to
// empty / zero on events that do not set them.
TEST(OnchainLifecycleTest, CexPathUnaffected)
{
  RecordingListener l;
  OrderEvent ev;
  ev.status = OrderEventStatus::FILLED;
  ev.dispatchTo(l);
  EXPECT_TRUE(l.filled);
  EXPECT_FALSE(l.pendingOnchain);
  EXPECT_FALSE(l.reverted);
  EXPECT_TRUE(ev.txHash.empty());
  EXPECT_EQ(ev.confirmations, 0u);
}

}  // namespace
