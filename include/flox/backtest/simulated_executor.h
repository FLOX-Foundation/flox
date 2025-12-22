/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/abstract_clock.h"
#include "flox/book/book_update.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/composite_order_logic.h"
#include "flox/execution/events/order_event.h"

#include <array>
#include <functional>
#include <vector>

namespace flox
{

struct Fill
{
  OrderId orderId{};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity quantity{};
  UnixNanos timestampNs{};
};

struct TrailingState
{
  Price activationPrice{};  // price when trailing stop was activated
  Price currentTrigger{};   // current trigger price (moves with price)
};

class SimulatedExecutor : public IOrderExecutor
{
 public:
  static constexpr size_t kMaxSymbols = 256;
  static constexpr size_t kDefaultOrderCapacity = 64;
  static constexpr size_t kDefaultFillCapacity = 4096;

  using OrderEventCallback = std::function<void(const OrderEvent&)>;

  explicit SimulatedExecutor(IClock& clock);

  void setOrderEventCallback(OrderEventCallback cb);

  void start() override {}
  void stop() override {}

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void cancelAllOrders(SymbolId symbol) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  // OCO: one-cancels-other
  void submitOCO(const OCOParams& params) override;

  ExchangeCapabilities capabilities() const override { return ExchangeCapabilities::simulated(); }

  void onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                    const std::pmr::vector<BookLevel>& asks);
  void onTrade(SymbolId symbol, Price price, bool isBuy);

  const std::vector<Fill>& fills() const noexcept { return _fills; }
  const std::vector<Order>& conditionalOrders() const noexcept { return _conditional_orders; }

  CompositeOrderLogic& compositeLogic() noexcept { return _compositeLogic; }

 private:
  struct MarketState
  {
    int64_t bestBidRaw{0};
    int64_t bestAskRaw{0};
    int64_t lastTradeRaw{0};
    bool hasBid{false};
    bool hasAsk{false};
    bool hasTrade{false};
  };

  MarketState& getMarketState(SymbolId symbol) noexcept;
  bool tryFillOrder(Order& order);
  void processPendingOrders(SymbolId symbol, const MarketState& state);
  void processConditionalOrders(SymbolId symbol, const MarketState& state);
  void updateTrailingStops(SymbolId symbol, Price currentPrice);
  bool checkStopTrigger(const Order& order, const MarketState& state) const;
  bool checkTakeProfitTrigger(const Order& order, const MarketState& state) const;
  bool checkTrailingStopTrigger(const Order& order, const TrailingState& trailing,
                                const MarketState& state) const;
  void triggerConditionalOrder(Order& order);
  bool isConditionalOrder(OrderType type) const;
  void executeFill(Order& order, Price price, Quantity qty);
  void emitEvent(OrderEventStatus status, const Order& order);
  void emitTrailingUpdate(const Order& order, Price newTrigger);

  IClock& _clock;
  OrderEventCallback _callback;

  std::vector<Order> _pending_orders;
  std::vector<Order> _conditional_orders;
  std::vector<Fill> _fills;

  // Trailing stop state tracking (indexed by order id)
  std::vector<std::pair<OrderId, TrailingState>> _trailing_states;

  // OCO order logic
  CompositeOrderLogic _compositeLogic{0};

  std::array<MarketState, kMaxSymbols> _marketStatesFlat{};
  std::vector<std::pair<SymbolId, MarketState>> _marketStatesOverflow;
};

}  // namespace flox
