/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include "lrvx/execution/abstract_executor.h"
#include "lrvx/run/trace_handlers.h"
#include "lrvx/strategy/abstract_signal_handler.h"
#include "lrvx/strategy/shadow.h"

#include <vector>

using namespace lrvx;

namespace
{

// An executor that overrides nothing batch-related: the base-class default
// must walk the batch through submitOrder, in order, one call per order.
class SingleOnlyExecutor : public IOrderExecutor
{
 public:
  void submitOrder(const Order& order) override { submittedIds.push_back(order.id); }

  std::vector<OrderId> submittedIds;
};

// An executor with a "native batch endpoint": one submitOrders call must
// arrive as one batch, and must NOT be rerouted through submitOrder.
class BatchingExecutor : public IOrderExecutor
{
 public:
  void submitOrder(const Order& order) override { singleSubmits.push_back(order.id); }

  void submitOrders(std::span<const Order> orders) override
  {
    std::vector<OrderId> batch;
    batch.reserve(orders.size());
    for (const auto& order : orders)
    {
      batch.push_back(order.id);
    }
    batches.push_back(std::move(batch));
  }

  ExchangeCapabilities capabilities() const override
  {
    auto caps = ExchangeCapabilities::none();
    caps.supportsBatchSubmit = true;
    return caps;
  }

  std::vector<OrderId> singleSubmits;
  std::vector<std::vector<OrderId>> batches;
};

class SingleOnlyHandler : public ISignalHandler
{
 public:
  void onSignal(const Signal& signal) override { seen.push_back(signal.orderId); }

  std::vector<OrderId> seen;
};

class BatchingHandler : public ISignalHandler
{
 public:
  void onSignal(const Signal& signal) override { singles.push_back(signal.orderId); }

  void onSignalBatch(std::span<const Signal> signals) override
  {
    batchSizes.push_back(signals.size());
  }

  std::vector<OrderId> singles;
  std::vector<size_t> batchSizes;
};

std::vector<Order> threeOrders()
{
  std::vector<Order> orders(3);
  orders[0].id = 11;
  orders[1].id = 22;
  orders[2].id = 33;
  return orders;
}

}  // namespace

TEST(BatchSubmitHooks, ExecutorDefaultLoopsSubmitOrderInOrder)
{
  SingleOnlyExecutor exec;
  const auto orders = threeOrders();

  exec.submitOrders(orders);

  ASSERT_EQ(exec.submittedIds.size(), 3u);
  EXPECT_EQ(exec.submittedIds[0], 11u);
  EXPECT_EQ(exec.submittedIds[1], 22u);
  EXPECT_EQ(exec.submittedIds[2], 33u);
}

TEST(BatchSubmitHooks, ExecutorDefaultOnEmptyBatchSubmitsNothing)
{
  SingleOnlyExecutor exec;

  exec.submitOrders({});

  EXPECT_TRUE(exec.submittedIds.empty());
}

TEST(BatchSubmitHooks, OverridingExecutorReceivesTheWholeBatchOnce)
{
  BatchingExecutor exec;
  const auto orders = threeOrders();

  // Through the interface, as a router would call it.
  IOrderExecutor& iface = exec;
  iface.submitOrders(orders);

  EXPECT_TRUE(exec.singleSubmits.empty());
  ASSERT_EQ(exec.batches.size(), 1u);
  EXPECT_EQ(exec.batches[0], (std::vector<OrderId>{11, 22, 33}));
}

TEST(BatchSubmitHooks, BatchCapabilityIsOptIn)
{
  SingleOnlyExecutor plain;
  BatchingExecutor batching;

  // The base advertises nothing; a looping default is not a native batch
  // endpoint, so the flag stays down until an adapter raises it.
  EXPECT_FALSE(plain.capabilities().supportsBatchSubmit);
  EXPECT_TRUE(batching.capabilities().supportsBatchSubmit);
  EXPECT_FALSE(ExchangeCapabilities::none().supportsBatchSubmit);
  EXPECT_TRUE(ExchangeCapabilities::all().supportsBatchSubmit);
}

TEST(BatchSubmitHooks, HandlerDefaultLoopsOnSignalInOrder)
{
  SingleOnlyHandler handler;
  std::vector<Signal> signals(2);
  signals[0].orderId = 7;
  signals[1].orderId = 8;

  handler.onSignalBatch(signals);

  ASSERT_EQ(handler.seen.size(), 2u);
  EXPECT_EQ(handler.seen[0], 7u);
  EXPECT_EQ(handler.seen[1], 8u);
}

TEST(BatchSubmitHooks, OverridingHandlerSeesOneBatchNotNSingles)
{
  BatchingHandler handler;
  std::vector<Signal> signals(4);

  ISignalHandler& iface = handler;
  iface.onSignalBatch(signals);

  EXPECT_TRUE(handler.singles.empty());
  ASSERT_EQ(handler.batchSizes.size(), 1u);
  EXPECT_EQ(handler.batchSizes[0], 4u);
}

// The in-tree wrapper handlers sit between a strategy and its real handler.
// If a wrapper fell back to the looping default, one coalesced submit would
// reach the batching bridge behind it as N singles — silently, since every
// signal still arrives.

TEST(BatchSubmitHooks, TraceWrapperKeepsTheBatchWhole)
{
  BatchingHandler bridge;
  run::TraceSignalHandler tracer(&bridge, /*recorder=*/nullptr);
  std::vector<Signal> signals(3);

  ISignalHandler& iface = tracer;
  iface.onSignalBatch(signals);

  EXPECT_TRUE(bridge.singles.empty());
  ASSERT_EQ(bridge.batchSizes.size(), 1u);
  EXPECT_EQ(bridge.batchSizes[0], 3u);
}

TEST(BatchSubmitHooks, ShadowWrapperRecordsEachAndForwardsTheBatchWhole)
{
  BatchingHandler bridge;
  ShadowSignalHandler shadow(/*maxRecords=*/16, &bridge);
  std::vector<Signal> signals(3);

  ISignalHandler& iface = shadow;
  iface.onSignalBatch(signals);

  EXPECT_EQ(shadow.records().size(), 3u);
  EXPECT_TRUE(bridge.singles.empty());
  ASSERT_EQ(bridge.batchSizes.size(), 1u);
  EXPECT_EQ(bridge.batchSizes[0], 3u);
}
