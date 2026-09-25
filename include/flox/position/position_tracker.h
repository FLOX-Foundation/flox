/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/position/abstract_position_manager.h"
#include "flox/strategy/symbol_state_map.h"
#include "flox/util/base/scale_check.h"

#include <cstdlib>
#include <deque>
#include <mutex>

namespace flox
{

enum class CostBasisMethod
{
  FIFO,
  LIFO,
  AVERAGE
};

struct Lot
{
  Quantity quantity;
  Price price;
};

namespace detail
{

// The weighted average of a set of prices, exact to the raw.
//
// The obvious form -- add up `quantity * price` as Volume, divide by the total
// quantity -- rounds every term to 1e-8 before adding it, so a thousand fills
// at one price report an average that is not that price, and the error depends
// on how the fills happened to be split. Doing it in double is worse: the
// result then also depends on the compiler's FMA contraction.
//
// Each product is carried as its quotient and its remainder against the scale
// instead. The remainder is exact in int64 -- both factors are below the scale
// after the modulo, so their product is below 1e16 -- and every whole scale it
// accumulates is handed back to the quotient, so the pair holds the entire
// product sum with nothing dropped. The single division at the end is the only
// rounding step, and it is the one that is unavoidable: an average price need
// not be representable at 1e-8.
//
// Weights are magnitudes (a short lot weighs the same as a long one), which is
// what a cost basis means; prices are taken as given.
class WeightedPriceSum
{
 public:
  void add(Quantity qty, Price price)
  {
    const int64_t q = qty.raw() < 0 ? -qty.raw() : qty.raw();
    const int64_t p = price.raw();
    _weight = checkedAddI64(_weight, q);

    const int64_t quotient = mulDivI64(q, p, Price::Scale);
    // q * p is exact modulo 2^64 even where it overflows int64, and so is
    // subtracting the part already counted; what is left is the remainder,
    // which is known to fit. mulDivI64 truncates toward zero, so a negative
    // product leaves a negative remainder -- normalise it to the floor form,
    // which is the one that carries.
    int64_t remainder = static_cast<int64_t>(static_cast<uint64_t>(q) * static_cast<uint64_t>(p) -
                                             static_cast<uint64_t>(quotient) *
                                                 static_cast<uint64_t>(Price::Scale));
    int64_t units = quotient;
    if (remainder < 0)
    {
      remainder += Price::Scale;
      units -= 1;
    }
    _units = checkedAddI64(_units, units);
    _fraction += remainder;
    if (_fraction >= Price::Scale)
    {
      _fraction -= Price::Scale;
      _units = checkedAddI64(_units, 1);
    }
  }

  bool empty() const { return _weight == 0; }

  Price average() const
  {
    if (_weight == 0)
    {
      return Price{};
    }
    // floor(_units * Scale / _weight), plus what that division left behind
    // together with the carried fraction -- the two of them are what tells a
    // thousand fills at one price from an average that merely rounds to it.
    int64_t whole = mulDivI64(_units, Price::Scale, _weight);
    int64_t leftover =
        static_cast<int64_t>(static_cast<uint64_t>(_units) * static_cast<uint64_t>(Price::Scale) -
                             static_cast<uint64_t>(whole) * static_cast<uint64_t>(_weight));
    if (leftover < 0)
    {
      leftover += _weight;
      whole -= 1;
    }
    const uint64_t tail = (static_cast<uint64_t>(leftover) + static_cast<uint64_t>(_fraction)) /
                          static_cast<uint64_t>(_weight);
    return Price::fromRaw(checkedAddI64(whole, static_cast<int64_t>(tail)));
  }

 private:
  int64_t _weight{0};    // sum of |quantity| raws
  int64_t _units{0};     // sum of the products, in Volume raws
  int64_t _fraction{0};  // what the products left below one Volume raw
};

}  // namespace detail

struct PositionState
{
  std::deque<Lot> lots;
  Volume realizedPnl{};

  Quantity position() const
  {
    int64_t total = 0;
    for (const auto& lot : lots)
    {
      total = checkedAddI64(total, lot.quantity.raw());
    }
    return Quantity::fromRaw(total);
  }

  Price avgEntryPrice() const
  {
    detail::WeightedPriceSum sum;
    for (const auto& lot : lots)
    {
      sum.add(lot.quantity, lot.price);
    }
    return sum.average();
  }

  // The two answers above from one pass over the lots. position() sums every
  // lot and avgEntryPrice() walks them all again, so a caller that wants both
  // -- which is every caller on the market-data path -- paid the traversal
  // twice. Same arithmetic as the separate getters, WeightedPriceSum included,
  // so the numbers are the same to the last raw unit.
  PositionSnapshot snapshot() const
  {
    int64_t totalRaw = 0;
    detail::WeightedPriceSum sum;
    for (const auto& lot : lots)
    {
      totalRaw = checkedAddI64(totalRaw, lot.quantity.raw());
      sum.add(lot.quantity, lot.price);
    }

    PositionSnapshot snap;
    snap.position = Quantity::fromRaw(totalRaw);
    // Nothing when flat, for the same reason getAverageEntryPrice() reports
    // nothing: there is no entry price to report and zero would read as one.
    if (totalRaw != 0 && !sum.empty())
    {
      snap.avgEntryPrice = sum.average();
    }
    return snap;
  }
};

class PositionTracker : public IPositionManager
{
 public:
  PositionTracker(SubscriberId id, CostBasisMethod method = CostBasisMethod::FIFO)
      : IPositionManager(id), _method(method)
  {
  }

  void start() override {}
  void stop() override {}

  CostBasisMethod method() const { return _method; }

  Quantity getPosition(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].position();
  }

  Price getAvgEntryPrice(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].avgEntryPrice();
  }

  // Nothing when the position is flat: there is no entry price to report, and
  // zero would read as a real one.
  std::optional<Price> getAverageEntryPrice(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto& state = _states[symbol];
    if (state.position().raw() == 0)
    {
      return std::nullopt;
    }
    return state.avgEntryPrice();
  }

  // One lock, one traversal, both answers.
  PositionSnapshot positionSnapshot(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].snapshot();
  }

  // Money, not a price: a quantity times a price difference is a notional, and
  // the engine spells that Volume. Typed as Price it compared equal to, and
  // added to, an actual price without a diagnostic.
  Volume getRealizedPnl(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].realizedPnl;
  }

  Volume getTotalRealizedPnl() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t total = 0;
    _states.forEach([&total](SymbolId, const PositionState& s)
                    { total = checkedAddI64(total, s.realizedPnl.raw()); });
    return Volume::fromRaw(total);
  }

  // Number of symbols with recorded state. Exists so a caller (and the
  // test for it) can observe that a pure query never grows this -- see the
  // note on `_states` below. Not part of IPositionManager; this is a
  // diagnostic, not something strategy code should branch on.
  size_t trackedSymbolCount() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states.size();
  }

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    updatePosition(order.symbol, order.side, order.price, fillQty);
  }

  void onOrderFilled(const Order& order) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    updatePosition(order.symbol, order.side, order.price, order.quantity);
  }

  // Preferred forms: they carry the price the fill actually happened at. A
  // market order has none on the order itself, so without these the cost basis
  // comes out as zero.
  void onOrderPartiallyFilled(const Order& order, Quantity fillQty,
                              Price fillPrice) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    updatePosition(order.symbol, order.side, fillPrice, fillQty);
  }

  void onOrderFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    updatePosition(order.symbol, order.side, fillPrice, fillQty);
  }

 private:
  void updatePosition(SymbolId symbol, Side side, Price price, Quantity qty)
  {
    auto& s = _states[symbol];
    int64_t signedQty = (side == Side::BUY) ? qty.raw() : -qty.raw();
    int64_t currentPos = s.position().raw();

    bool isOpening = (currentPos >= 0 && signedQty > 0) || (currentPos <= 0 && signedQty < 0);
    bool isClosing = (currentPos > 0 && signedQty < 0) || (currentPos < 0 && signedQty > 0);

    if (isClosing)
    {
      int64_t absSignedQty = std::abs(signedQty);
      int64_t absCurrentPos = std::abs(currentPos);
      int64_t qtyToClose = std::min(absSignedQty, absCurrentPos);
      Volume pnl = closePosition(s, Quantity::fromRaw(qtyToClose), price, currentPos > 0);
      s.realizedPnl = Volume::fromRaw(checkedAddI64(s.realizedPnl.raw(), pnl.raw()));

      int64_t remaining = absSignedQty - qtyToClose;
      if (remaining > 0)
      {
        Quantity remQty = Quantity::fromRaw(signedQty > 0 ? remaining : -remaining);
        addLot(s, remQty, price);
      }
    }
    else if (isOpening)
    {
      addLot(s, Quantity::fromRaw(signedQty), price);
    }
  }

  void addLot(PositionState& s, Quantity signedQty, Price price)
  {
    if (_method == CostBasisMethod::AVERAGE && !s.lots.empty())
    {
      int64_t newQty = checkedAddI64(s.lots.front().quantity.raw(), signedQty.raw());
      if (newQty != 0)
      {
        // The running average is re-derived on every fill, so this is the one
        // step whose rounding compounds over a session. Re-derive it from the
        // two weighted prices exactly rather than from the previous average's
        // rounded form.
        detail::WeightedPriceSum sum;
        sum.add(s.lots.front().quantity, s.lots.front().price);
        sum.add(signedQty, price);
        s.lots.front().quantity = Quantity::fromRaw(newQty);
        s.lots.front().price = sum.average();
      }
      else
      {
        s.lots.clear();
      }
    }
    else
    {
      s.lots.push_back({signedQty, price});
    }
  }

  Volume closePosition(PositionState& s, Quantity qtyToClose, Price closePrice, bool wasLong)
  {
    int64_t pnl = 0;
    int64_t remaining = qtyToClose.raw();

    while (remaining > 0 && !s.lots.empty())
    {
      Lot& lot = (_method == CostBasisMethod::LIFO) ? s.lots.back() : s.lots.front();

      int64_t lotQty = std::abs(lot.quantity.raw());
      int64_t closeQty = std::min(remaining, lotQty);

      // One realisation per lot, in the same fixed point the fills arrived in.
      // The old form computed the price difference and the product as doubles
      // and converted back, which drifts once a price outgrows the mantissa and
      // drifts by a different amount depending on how the compiler contracted
      // the multiply.
      const int64_t priceDiff = checkedSubI64(closePrice.raw(), lot.price.raw());
      int64_t lotPnl = mulDivI64(priceDiff, closeQty, Volume::Scale);
      if (!wasLong)
      {
        lotPnl = -lotPnl;
      }
      pnl = checkedAddI64(pnl, lotPnl);

      remaining -= closeQty;

      if (closeQty >= lotQty)
      {
        if (_method == CostBasisMethod::LIFO)
        {
          s.lots.pop_back();
        }
        else
        {
          s.lots.pop_front();
        }
      }
      else
      {
        int64_t newQty = (lot.quantity.raw() > 0) ? (lotQty - closeQty) : -(lotQty - closeQty);
        lot.quantity = Quantity::fromRaw(newQty);
      }
    }

    return Volume::fromRaw(pnl);
  }

  CostBasisMethod _method;
  mutable std::mutex _mutex;
  // Not mutable. Every write goes through a non-const method (updatePosition
  // and friends), so this never needs to change from a const one -- and it
  // must not appear to, either: `mutable` here used to make the const query
  // methods below pick SymbolStateMap's non-const operator[] (a mutable
  // member is never const, regardless of the enclosing method), which marks
  // the symbol initialized on a plain read. A symbol that never traded then
  // showed up in size()/forEach()/getTotalRealizedPnl() just because
  // someone asked about it. Leaving this non-mutable makes the const
  // methods bind the const overload instead, which never marks anything.
  SymbolStateMap<PositionState> _states;
};

}  // namespace flox
