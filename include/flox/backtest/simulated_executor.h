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
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/bracket_order.h"
#include "flox/backtest/latency_distribution.h"
#include "flox/backtest/order_queue_tracker.h"
#include "flox/backtest/rate_limit_policy.h"
#include "flox/backtest/venue_availability.h"
#include "flox/book/book_update.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/composite_order_logic.h"
#include "flox/execution/events/order_event.h"

#include <array>
#include <functional>
#include <random>
#include <unordered_map>
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

  // Apply slippage and queue-simulation settings from a BacktestConfig. May be
  // called after construction; existing orders keep their prior behavior.
  void applyConfig(const BacktestConfig& config);

  // Convenience setters for bindings without a full BacktestConfig.
  void setDefaultSlippage(const SlippageProfile& profile);
  void setSymbolSlippage(SymbolId symbol, const SlippageProfile& profile);
  void setQueueModel(QueueModel model, size_t depth);
  // For QueueModel::PRO_RATA_WITH_FIFO: the first N orders at a
  // level consume the trade FIFO; the remainder is split pro-rata
  // across the rest. Ignored in other queue models.
  void setQueueFifoTopN(size_t topN);
  // TOP_PRO_LMM: fraction of each incoming trade reserved for the
  // queue-front order. Default 0.40. Ignored in other queue models.
  void setTopPriorityShare(double share);
  // TOP_PRO_LMM: identify these orders as Lead Market Makers. LMM
  // orders get an implicit bonus multiplier during the pro-rata
  // remainder distribution.
  void setLmmOrders(const std::vector<OrderId>& ids);
  void setLmmBonusMultiplier(double multiplier);
  // TOP_PRO_LMM / PRO_RATA_WITH_PRIORITY: per-order priority weight.
  // Defaults to 1.0. Effective allocation weight in pro-rata
  // distribution = remaining × priorityMultiplier.
  void setOrderPriorityMultiplier(OrderId id, double multiplier);
  void setQueuePositionMinChangeFraction(double fraction);
  void setSubmitAckLatency(int64_t latencyNs, int64_t jitterNs);
  void setCancelAckLatency(int64_t latencyNs, int64_t jitterNs);
  void setReplaceAckLatency(int64_t latencyNs, int64_t jitterNs);

  // Distribution-based variants. The scalar setters above delegate
  // to these: zero jitter → Constant, non-zero → Uniform with the
  // legacy [base-jitter, base+jitter] window.
  void setSubmitAckLatencyDistribution(const LatencyDistribution& dist);
  void setCancelAckLatencyDistribution(const LatencyDistribution& dist);
  void setReplaceAckLatencyDistribution(const LatencyDistribution& dist);

  void applyLatencyProfile(const char* name);

  // Self-trade prevention. When set, submitOrder consults pending
  // resting orders and applies the configured mode if the incoming
  // order would cross one of the same account's resting orders.
  // Multi-account workflows: STP keys on (Order::accountId, optional
  // STP group). Two orders share an STP scope when either their
  // accountIds match, or their accounts belong to the same explicit
  // STP group (configure via setSTPGroupMembership).
  void setSTPMode(STPMode mode) noexcept { _stpMode = mode; }
  STPMode stpMode() const noexcept { return _stpMode; }

  // Opt an account into an STP group. Group id 0 removes any prior
  // mapping. Two orders belong to the same STP scope when either
  // their accountIds match (and are equal), or both accounts map to
  // the same non-zero group.
  void setSTPGroupMembership(uint64_t accountId, uint64_t groupId);
  uint64_t stpGroupFor(uint64_t accountId) const;
  // True when the simulator should treat two orders as candidates for
  // STP (same direct account, or both in the same non-zero group).
  bool sameStpScope(uint64_t a, uint64_t b) const;

  // FOK fill semantics. Real venues split:
  //   AnyPrice    — fill if cumulative liquidity along the book (at
  //                 prices crossing the order's limit) >= order qty.
  //                 Walks through multiple price levels.
  //   SinglePrice — fill if the level at the order's limit price
  //                 holds >= order qty in one trade. Reject otherwise.
  // Default is AnyPrice (matches crypto venues). CME / Eurex / most
  // US equities use SinglePrice. The simulator currently consults
  // top-of-book qty only; deeper book walks are a follow-up.
  enum class FokMode : uint8_t
  {
    AnyPrice = 0,
    SinglePrice = 1,
  };
  void setFokMode(FokMode mode) noexcept { _fokMode = mode; }
  FokMode fokMode() const noexcept { return _fokMode; }
  // String form for codegen / bindings. Accepts "any_price" or
  // "single_price" (case-insensitive). Unknown values are ignored.
  void setFokModeByName(const std::string& name);

  // Attach a venue-availability model. Submit / cancel / replace
  // issued while the venue is down are buffered and flushed at the
  // recovery edge in FIFO order. Market-data callbacks (onTrade /
  // onBookUpdate / onBar) are silently dropped during an outage so
  // the strategy sees a feed gap. Passing nullptr disables outages.
  void setVenueAvailability(VenueAvailability* availability) { _venue = availability; }
  VenueAvailability* venueAvailability() noexcept { return _venue; }

  // Attach a rate-limit policy. Submit / cancel / replace consult the
  // policy first; an overflow emits OrderEventStatus::REJECTED_RATE_LIMIT
  // and the action is not committed to the simulator. Passing an
  // empty policy disables enforcement.
  void setRateLimitPolicy(const RateLimitPolicy& policy);
  void clearRateLimitPolicy();
  bool hasRateLimitPolicy() const noexcept { return _hasRateLimit; }
  RateLimitPolicy& rateLimitPolicy() { return _rateLimit; }
  const RateLimitPolicy& rateLimitPolicy() const { return _rateLimit; }

  void start() override {}
  void stop() override {}

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void cancelAllOrders(SymbolId symbol) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  // OCO: one-cancels-other
  void submitOCO(const OCOParams& params) override;

  // Native bracket primitive: a single call that submits the entry
  // leg and arms a take-profit + stop pair on entry fill. The
  // simulator manages the state machine: the first child to fill
  // cancels the other; cancelling the bracket cancels every still-
  // live leg. Returns the bracketId for later lookup / cancel.
  // The bracket assumes order_ids for the entry / TP / stop are
  // chosen by the caller: entry_id = bracketId * 3 + 0,
  // tp_id = bracketId * 3 + 1, stop_id = bracketId * 3 + 2.
  void submitBracket(const BracketOrder& bracket);
  void cancelBracket(uint64_t bracketId);
  BracketStatus bracketStatus(uint64_t bracketId) const;

  ExchangeCapabilities capabilities() const override { return ExchangeCapabilities::simulated(); }

  void onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                    const std::pmr::vector<BookLevel>& asks);
  void onTrade(SymbolId symbol, Price price, bool isBuy);
  void onTrade(SymbolId symbol, Price price, Quantity qty, bool isBuy);
  void onBar(SymbolId symbol, Price close);

  const std::vector<Fill>& fills() const { return _fills; }
  std::vector<Fill> extractFills() { return std::move(_fills); }
  const std::vector<Order>& conditionalOrders() const { return _conditional_orders; }

  // Top-of-book accessors. Return 0 when the side is empty.
  Price bestBidPrice(SymbolId symbol) const;
  Price bestAskPrice(SymbolId symbol) const;
  // Mid of best bid and best ask. Returns 0 when either side is empty.
  Price bookMidPrice(SymbolId symbol) const;

  CompositeOrderLogic& compositeLogic() { return _compositeLogic; }

 private:
  struct MarketState
  {
    int64_t bestBidRaw{0};
    int64_t bestAskRaw{0};
    int64_t lastTradeRaw{0};
    int64_t bestBidQtyRaw{0};
    int64_t bestAskQtyRaw{0};
    bool hasBid{false};
    bool hasAsk{false};
    bool hasTrade{false};
  };

  MarketState& getMarketState(SymbolId symbol);
  const SlippageProfile& slippageFor(SymbolId symbol) const;
  int64_t applySlippage(int64_t priceRaw, Side side, SymbolId symbol,
                        Quantity qty, int64_t levelQtyRaw) const;

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
  void executeFill(Order& order, Price price, Quantity qty, bool isMaker = false);
  void emitEvent(OrderEventStatus status, const Order& order);
  void emitTrailingUpdate(const Order& order, Price newTrigger);

  // Emit QUEUE_POSITION_UPDATED for every resting order whose
  // `queueAhead` has shifted by at least the configured fractional
  // threshold since the last emission. Call after any operation
  // that may shift queue positions (onTrade, onBookUpdate, addOrder).
  void maybeEmitQueuePositionChanges();
  void forgetQueuePosition(OrderId orderId);

  // Native iceberg refresh. If `order` has an iceberg state, the
  // visible tranche is fully filled, the refresh deadline has
  // elapsed, and there is hidden remainder, expose another visible
  // tranche by increasing `order.quantity` by the lesser of the
  // hidden remainder and the configured visible slice. Re-adds the
  // refreshed slice to the queue tracker. No-op for non-iceberg
  // orders or when the refresh isn't due yet.
  void maybeRefreshIceberg(Order& order);

  // Asynchronous cancel ack support. enqueuePendingCancel records the
  // ack deadline; finalizePendingCancels walks the queue and CANCELs
  // any whose deadline has passed. resolveLateCancelOnFill drops a
  // pending cancel when its order has just filled and emits the
  // late-cancel-after-fill REJECTED notification.
  void enqueuePendingCancel(const Order& order);
  void finalizePendingCancels();
  void resolveLateCancelOnFill(const Order& order);
  int64_t sampleCancelAckLatency();

  // Market position tracking. Recomputes MarketPosition for every
  // resting limit order after a book or trade event and emits
  // MARKET_POSITION_CHANGED on categorical transitions.
  void maybeEmitMarketPositionChanges();
  MarketPosition computeMarketPosition(const Order& order,
                                       const MarketState& state,
                                       Quantity ourRemaining,
                                       Quantity queueTotal) const;
  int32_t computeDistanceToBestTicks(const Order& order,
                                     const MarketState& state) const;
  void forgetMarketPosition(OrderId orderId);

  // Replace-in-flight ack: enqueue pending replace, finalize when
  // ack deadline passes, reject the replace when the original
  // order fills first.
  void enqueuePendingReplace(const Order& oldOrder, const Order& newOrder);
  void finalizePendingReplaces();
  void resolveLateReplaceOnFill(const Order& order);
  int64_t sampleReplaceAckLatency();
  void forgetPendingReplace(OrderId orderId);

  // Submit ack: when cfg.submitAckLatencyNs > 0, SUBMITTED fires
  // immediately, ACCEPTED defers until the sampled deadline. The
  // order is held aside until ACCEPTED, then runs through the
  // existing book-add / queue-tracker / try-fill path.
  void enqueuePendingSubmission(const Order& order);
  void finalizePendingSubmissions();
  int64_t sampleSubmitAckLatency();
  void finishSubmission(Order accepted, bool fromAck);

  Order* findPendingOrder(OrderId orderId);
  void drainQueueFills(SymbolId symbol);

  // GTD: expire any pending order whose expiresAfter has passed. Called
  // alongside the ack finalizers from onBookUpdate / onTrade so the
  // expiry fires deterministically at the next event boundary.
  void processExpiredOrders();

  // reduce_only enforcement: simulator-side net position per symbol,
  // updated in executeFill. A reduce-only submit that would open or
  // grow the position is rejected; one that overshoots is truncated.
  std::unordered_map<SymbolId, int64_t> _netPositionRaw;
  int64_t netPositionRaw(SymbolId symbol) const;

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

  // Slippage config: default + per-symbol overrides
  std::array<SlippageProfile, kMaxSymbols> _slippageFlat{};
  std::array<bool, kMaxSymbols> _slippageSetFlat{};
  std::vector<std::pair<SymbolId, SlippageProfile>> _slippageOverflow;
  SlippageProfile _defaultSlippage{};

  OrderQueueTracker _queueTracker;
  QueueModel _queueModel{};
  std::vector<std::pair<OrderId, Quantity>> _queueFillBuffer;  // reused scratch
  std::vector<QueueSnapshot> _queueSnapshotBuffer;             // reused scratch
  std::unordered_map<OrderId, int64_t> _lastEmittedQueueAheadRaw;
  double _queuePosMinFraction{0.05};

  // Per-order lifecycle-stage timestamps. Populated as statuses fire
  // (SUBMITTED, ACCEPTED, fills, CANCELED, REJECTED, TRIGGERED,
  // EXPIRED). Each emitted OrderEvent receives the current snapshot.
  std::unordered_map<OrderId, OrderTimestamps> _orderTimestamps;

  OrderTimestamps& timestampsFor(OrderId id);
  void forgetTimestamps(OrderId id);

  struct PendingCancel
  {
    OrderId orderId;
    int64_t ackAtNs;
    Order orderSnapshot;  // copy at the moment of cancelOrder()
  };
  std::vector<PendingCancel> _pendingCancels;
  LatencyDistribution _cancelAckDist;
  std::mt19937_64 _cancelAckRng{42};

  // Last emitted MarketPosition per resting order. Used to debounce
  // MARKET_POSITION_CHANGED emission so it fires only on categorical
  // transitions, not on every tick.
  std::unordered_map<OrderId, MarketPosition> _lastEmittedMarketPosition;

  struct PendingReplace
  {
    OrderId orderId;
    int64_t ackAtNs;
    Order oldSnapshot;
    Order newOrder;
  };
  std::vector<PendingReplace> _pendingReplaces;
  LatencyDistribution _replaceAckDist;

  struct PendingSubmission
  {
    int64_t ackAtNs;
    Order order;
  };
  std::vector<PendingSubmission> _pendingSubmissions;
  LatencyDistribution _submitAckDist;

  RateLimitPolicy _rateLimit;
  bool _hasRateLimit{false};

  STPMode _stpMode{STPMode::None};
  std::unordered_map<uint64_t, uint64_t> _stpGroupMembership;
  FokMode _fokMode{FokMode::AnyPrice};

  VenueAvailability* _venue{nullptr};
  enum class BufferedAction : uint8_t
  {
    SUBMIT = 0,
    CANCEL = 1,
    REPLACE = 2,
    CANCEL_ALL_SYMBOL = 3,
  };
  struct BufferedRequest
  {
    BufferedAction action{BufferedAction::SUBMIT};
    Order order{};
    OrderId oldOrderId{0};
    SymbolId cancelAllSymbol{0};
  };
  std::vector<BufferedRequest> _outageBuffer;
  void applyOutagePolicy();
  void flushOutageBuffer();
  bool _venueWasUp{true};

  // Native bracket state. Keyed by user-provided bracketId.
  std::unordered_map<uint64_t, BracketStatus> _brackets;
  std::unordered_map<OrderId, uint64_t> _legToBracket;
  std::unordered_map<uint64_t, BracketOrder> _bracketTemplates;
  void onBracketFillEvent(const Order& filledOrder);

  // Native iceberg state. Keyed by parent OrderId. `hiddenRaw` is
  // the remaining hidden quantity that has not yet been exposed to
  // the book. `visibleRaw` is the slice size used on each refresh.
  // `refreshLatencyNs` is the configured delay between a visible
  // tranche filling and the next one being exposed (0 = instant).
  struct IcebergState
  {
    int64_t hiddenRaw{0};
    int64_t visibleRaw{0};
    int64_t refreshLatencyNs{0};
    int64_t refreshDueNs{0};  // when the next refresh becomes eligible
  };
  std::unordered_map<OrderId, IcebergState> _iceberg;
  int64_t _icebergRefreshLatencyNs{0};

 public:
  // T041: queue priority on refresh. "back" (default, T029) sends
  // the refreshed slice to the back of the queue. "retain" keeps the
  // refresh in the same queue position as the prior slice (CME-style).
  enum class IcebergPriorityMode : uint8_t
  {
    Back = 0,
    Retain = 1,
  };

 private:
  // T041: per-refresh size jitter as a fraction (0.0 disables;
  // 0.10 = ±10% uniform). Default 0 keeps T029 deterministic slicing.
  double _icebergSizeRandomisationPct{0.0};
  IcebergPriorityMode _icebergPriorityMode{IcebergPriorityMode::Back};
  std::mt19937_64 _icebergJitterRng{0xC0FFEEC0FFEEULL};

 public:
  // T040: child-arm policy. "on_full_fill" (default) arms TP+stop
  // once at the moment of full entry fill. "on_partial_fill" arms
  // children at the running entry-fill quantity on every partial,
  // resizing via replace as more entry quantity fills.
  enum class BracketArmMode : uint8_t
  {
    OnFullFill = 0,
    OnPartialFill = 1,
  };
  void setBracketChildArmMode(BracketArmMode mode) noexcept { _bracketArmMode = mode; }
  BracketArmMode bracketChildArmMode() const noexcept { return _bracketArmMode; }

  // Configure the default native-iceberg refresh latency (the delay
  // between a visible tranche fully filling and the next one being
  // exposed to the book). Applies to orders of type
  // OrderType::ICEBERG submitted after this call.
  void setIcebergRefreshLatency(int64_t latencyNs) noexcept
  {
    _icebergRefreshLatencyNs = latencyNs;
  }
  int64_t icebergRefreshLatencyNs() const noexcept { return _icebergRefreshLatencyNs; }

  // T041: per-refresh visible-slice size jitter as a fraction
  // (0.0 = deterministic = T029 behaviour; 0.10 = ±10% uniform).
  // The jitter is sampled from an internal RNG seeded deterministically;
  // call setIcebergJitterSeed to reproduce a specific draw sequence.
  void setIcebergSizeRandomisationPct(double pct) noexcept
  {
    _icebergSizeRandomisationPct = pct;
  }
  double icebergSizeRandomisationPct() const noexcept
  {
    return _icebergSizeRandomisationPct;
  }
  void setIcebergJitterSeed(uint64_t seed) noexcept
  {
    _icebergJitterRng.seed(seed);
  }
  // T041: queue priority on refresh.
  //   Back    — refreshed slice goes to the back of the queue (T029
  //             default; matches most crypto venues).
  //   Retain  — refreshed slice keeps the same queue position as the
  //             prior slice (CME options + some Eurex contracts).
  void setIcebergPriorityMode(IcebergPriorityMode mode) noexcept
  {
    _icebergPriorityMode = mode;
  }
  IcebergPriorityMode icebergPriorityMode() const noexcept
  {
    return _icebergPriorityMode;
  }
  void setIcebergPriorityModeByName(const std::string& name) noexcept;
  // Diagnostic: remaining hidden quantity for an iceberg order, or 0
  // if none.
  int64_t icebergHiddenRemainingRaw(OrderId id) const
  {
    auto it = _iceberg.find(id);
    return it == _iceberg.end() ? 0 : it->second.hiddenRaw;
  }

 private:
  BracketArmMode _bracketArmMode{BracketArmMode::OnFullFill};
};

}  // namespace flox
